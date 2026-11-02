// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2022 Axcient.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2022 by Delphix. All rights reserved.
 * Copyright (c) 2024, Klara, Inc.
 */

/*
 * illumos port notes (relative to the OpenZFS zstream-dedup-forwarding
 * branch):
 *   - zstd does not exist on illumos, so zstd support and the compression
 *     level option (-l) are removed. illumos encodes gzip levels in the
 *     algorithm itself: "gzip-1" ... "gzip-9" (and "gzip" == "gzip-6").
 *   - illumos' zio_compress_data(c, src_abd, dst_buf, s_len) compresses into
 *     a plain buffer and has no level argument; it returns 0 for an all-zero
 *     block and s_len when the data did not compress by at least 12.5%.
 *     Decompression uses zio_decompress_data_buf(), which avoids abds.
 *   - The libzpool subsystems are brought up by zstream_chain_exec(); the
 *     upstream abd_init()/zio_init()/zstd_init() calls are not needed.
 *   - Upstream used argv[0] both as the compression type and as the input
 *     file name; here the input file is the optional second argument.
 *   - drr_payloadlen is left untouched for WRITE records (it is only
 *     meaningful for BEGIN records); the payload length is carried in
 *     dp_payload_size and drr_compressed_size.
 */

#include <err.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/abd.h>
#include <sys/spa.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>
#include "zstream.h"
#include "zstream_compress.h"
#include "zstream_modules.h"
#include "zstream_shared.h"

#define	MAX_COMPRESSION_STEPS	4

static compression_spec_t	specs[MAX_COMPRESSION_STEPS];
static int			next_spec = 0;

/*
 * In order to recompress an encrypted block, you have to decrypt,
 * decompress, recompress, and re-encrypt. That can be a future
 * enhancement (along with decryption or re-encryption), but for now
 * we leave encrypted (raw) blocks alone.
 */
static boolean_t
write_is_encrypted(const struct drr_write *drrw)
{
	for (int i = 0; i < ZIO_DATA_SALT_LEN; i++) {
		if (drrw->drr_salt[i] != 0)
			return (B_TRUE);
	}
	for (int i = 0; i < ZIO_DATA_IV_LEN; i++) {
		if (drrw->drr_iv[i] != 0)
			return (B_TRUE);
	}
	for (int i = 0; i < ZIO_DATA_MAC_LEN; i++) {
		if (drrw->drr_mac[i] != 0)
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * In a send stream an uncompressed WRITE has drr_compressiontype == 0, which
 * is ZIO_COMPRESS_INHERIT in enum zio_compress, not ZIO_COMPRESS_OFF.
 * ZIO_COMPRESS_OFF itself appears only for uncompressed blocks in raw sends,
 * where the record must be passed through untouched. Either way such a
 * record must never be handed to a decompressor.
 */
#define	DRR_UNCOMPRESSED	0

static boolean_t
write_is_compressed(const struct drr_write *drrw)
{
	return (drrw->drr_compressiontype != DRR_UNCOMPRESSED &&
	    drrw->drr_compressiontype != ZIO_COMPRESS_OFF);
}

/*
 * We can ignore the context here because it's already been evaluated by the
 * cost function. If the cost function returned something other than zero,
 * we have to decompress.
 */
static void
chain_decompress_writes(drr_packet_t *item, void *context)
{
	(void) context;

	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw = &drr->drr_u.drr_write;
	enum zio_compress dtype = drrw->drr_compressiontype;
	uint8_t *buff = safe_calloc(drrw->drr_logical_size);

	VERIFY3U(drr->drr_type, ==, DRR_WRITE);
	VERIFY(write_is_compressed(drrw));
	if (zio_decompress_data_buf(dtype, item->dp_payload, buff,
	    item->dp_payload_size, drrw->drr_logical_size) != 0) {
		errx(4, "decompression type %d failed for ino %llu "
		    "offset %llu", dtype,
		    (u_longlong_t)drrw->drr_object,
		    (u_longlong_t)drrw->drr_offset);
	}
	free(item->dp_payload);
	item->dp_payload = buff;
	item->dp_payload_size = drrw->drr_logical_size;
	drrw->drr_compressed_size = 0;
	drrw->drr_compressiontype = DRR_UNCOMPRESSED;
}

static void
chain_compress_writes(drr_packet_t *item, compression_spec_t *context)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw = &drr->drr_u.drr_write;
	uint64_t lsize = drrw->drr_logical_size;
	uint8_t *buff = safe_calloc(lsize);
	size_t csize, rounded;
	abd_t *sabd;

	VERIFY3U(drr->drr_type, ==, DRR_WRITE);
	VERIFY3U(drrw->drr_compressiontype, ==, DRR_UNCOMPRESSED);
	VERIFY3U(item->dp_payload_size, ==, lsize);

	sabd = abd_get_from_buf(item->dp_payload, lsize);
	csize = zio_compress_data(context->cs_type, sabd, buff, lsize);
	abd_put(sabd);

	/*
	 * A zero return means the block is all zeros. A send stream has no
	 * way to express a zero-length compressed WRITE, so such blocks (and
	 * blocks that did not compress usefully) stay uncompressed.
	 */
	rounded = P2ROUNDUP(csize, SPA_MINBLOCKSIZE);
	if (csize != 0 && rounded < lsize) {
		bzero(buff + csize, rounded - csize);
		drrw->drr_compressiontype = context->cs_type;
		drrw->drr_compressed_size = rounded;
		free(item->dp_payload);
		item->dp_payload = buff;
		item->dp_payload_size = rounded;
	} else {
		free(buff);
	}
}

static size_t
chain_compress_cost(drr_packet_t *item, compression_spec_t *context)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw = &drr->drr_u.drr_write;

	if (drr->drr_type != DRR_WRITE || write_is_encrypted(drrw))
		return (0);
	if (context->cs_type == ZIO_COMPRESS_OFF ||
	    drrw->drr_compressiontype != DRR_UNCOMPRESSED)
		return (0);
	return (item->dp_payload_size);
}

static size_t
chain_decompress_cost(drr_packet_t *item, compression_spec_t *context)
{
	dmu_replay_record_t *drr = &item->dp_drr;
	struct drr_write *drrw = &drr->drr_u.drr_write;

	if (drr->drr_type != DRR_WRITE || !write_is_compressed(drrw) ||
	    write_is_encrypted(drrw))
		return (0);
	if (context == NULL || drrw->drr_compressiontype != context->cs_type)
		return (item->dp_payload_size);
	return (0);
}

static compression_spec_t *
next_context(void)
{
	compression_spec_t *context = &specs[next_spec % MAX_COMPRESSION_STEPS];
	next_spec++;
	return (context);
}

chain_step_t
parallel_decompress_writes(compression_spec_t *target)
{
	compression_spec_t *context = NULL;

	if (target != NULL) {
		context = next_context();
		*context = *target;
	}
	chain_step_t step = {
		.cs_type = CS_PARALLEL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.parallel = {
			.csp_queue_length = 128,
			.csp_batch_budget = 128 * 1024,
			.csp_process =
			    (zq_process_item_f *)chain_decompress_writes,
			.csp_cost = (zq_estimate_cost_f *)chain_decompress_cost,
			.csp_context = context
		}
	};
	return (step);
}

chain_step_t
parallel_compress_writes(compression_spec_t target)
{
	compression_spec_t *context = next_context();

	*context = target;
	chain_step_t step = {
		.cs_type = CS_PARALLEL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.parallel = {
			.csp_queue_length = 128,
			.csp_batch_budget = 128 * 1024,
			.csp_process =
			    (zq_process_item_f *)chain_compress_writes,
			.csp_cost = (zq_estimate_cost_f *)chain_compress_cost,
			.csp_context = context
		}
	};
	return (step);
}

/*
 * A receiver interprets compressed WRITE records only if the stream's BEGIN
 * record carries DMU_BACKUP_FEATURE_COMPRESSED (plus DMU_BACKUP_FEATURE_LZ4
 * for lz4), as "zfs send -c" sets them. Set them on every substream BEGIN
 * when recompressing, so that an uncompressed "zfs send" can be compressed.
 * As in zstream dedup, the DMU_COMPOUNDSTREAM header of a "-R"/"-I" package
 * is left byte-for-byte intact: its checksum lives in the following
 * toguid==0 END record, which is not recomputed.
 */
static boolean_t
chain_set_compress_features(drr_packet_t *item, compression_spec_t *context,
    chain_attrs_t attrs)
{
	(void) attrs;

	if (item == NULL || item->dp_drr.drr_type != DRR_BEGIN ||
	    context->cs_type == ZIO_COMPRESS_OFF)
		return (B_TRUE);

	struct drr_begin *drrb = &item->dp_drr.drr_u.drr_begin;
	if (DMU_GET_STREAM_HDRTYPE(drrb->drr_versioninfo) ==
	    DMU_COMPOUNDSTREAM)
		return (B_TRUE);

	uint64_t fflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
	fflags |= DMU_BACKUP_FEATURE_COMPRESSED;
	if (context->cs_type == ZIO_COMPRESS_LZ4)
		fflags |= DMU_BACKUP_FEATURE_LZ4;
	DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, fflags);
	return (B_TRUE);
}

static chain_step_t
serial_set_compress_features(compression_spec_t target)
{
	compression_spec_t *context = next_context();

	*context = target;
	chain_step_t step = {
		.cs_type = CS_SERIAL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.serial = {
			.css_process =
			    (zc_serial_process_f *)chain_set_compress_features,
			.css_context = context
		}
	};
	return (step);
}

/*
 * Map a user-supplied name to a compression type usable for WRITE records.
 * Returns B_FALSE for unknown names and for pseudo-types (inherit, on,
 * empty) that cannot describe a block's actual compression.
 */
static boolean_t
parse_compression_type(const char *name, enum zio_compress *typep)
{
	if (strcmp(name, "off") == 0) {
		*typep = ZIO_COMPRESS_OFF;
		return (B_TRUE);
	}
	if (strcmp(name, "gzip") == 0) {
		*typep = ZIO_COMPRESS_GZIP_6;
		return (B_TRUE);
	}
	for (enum zio_compress ct = 0; ct < ZIO_COMPRESS_FUNCTIONS; ct++) {
		if (ct == ZIO_COMPRESS_INHERIT || ct == ZIO_COMPRESS_ON ||
		    ct == ZIO_COMPRESS_EMPTY)
			continue;
		if (zio_compress_table[ct].ci_compress != NULL &&
		    strcmp(name, zio_compress_table[ct].ci_name) == 0) {
			*typep = ct;
			return (B_TRUE);
		}
	}
	return (B_FALSE);
}

int
zstream_do_recompress(int argc, char *argv[])
{
	int c;
	struct chain_attrs attrs = {0};

	while ((c = getopt(argc, argv, "v")) != -1) {
		switch (c) {
		case 'v':
			attrs.ca_flags |= CA_VERBOSE;
			break;
		case '?':
			(void) fprintf(stderr, "invalid option '%c'\n",
			    optopt);
			zstream_usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1 || argc > 2)
		zstream_usage();

	compression_spec_t spec;
	if (!parse_compression_type(argv[0], &spec.cs_type)) {
		(void) fprintf(stderr, "Invalid compression type %s.\n"
		    "Supported types are off, lzjb, gzip, gzip-[1-9], zle, "
		    "and lz4 (zstd is not supported on illumos)\n", argv[0]);
		exit(2);
	}

	const char *infile = (argc == 2) ? argv[1] : NULL;
	if (infile == NULL && isatty(STDIN_FILENO)) {
		(void) fprintf(stderr,
		    "Error: The send stream is a binary format "
		    "and can not be read from a\n"
		    "terminal.  Standard input must be redirected.\n");
		exit(1);
	}

	zstream_chain_t recompress_chain = {
		serial_read_stream(infile),
		parallel_calc_fletcher4(),
		serial_validate_fletcher4(),
		serial_byteswap(),
		parallel_validate_records(),
		parallel_decompress_writes(&spec),
		parallel_compress_writes(spec),
		serial_set_compress_features(spec),
		parallel_calc_fletcher4(),
		serial_add_fletcher4(),
		serial_write_stream(NULL)
	};

	zstream_chain_exec(recompress_chain, &attrs,
	    sizeof (recompress_chain) / sizeof (chain_step_t));

	return (0);
}
