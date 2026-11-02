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
 * Copyright (c) 2024, Klara, Inc.
 */

/*
 * zstream drop_records: remove selected WRITE and WRITE_EMBEDDED records
 * from a send stream, regenerating the stream checksums.
 *
 * illumos port: OpenZFS master implements this subcommand as a step in its
 * chain framework. The dedup-forwarding chain framework used by this port
 * has no way for a step to drop a record, so - like zstream decompress -
 * this is a standalone record loop with the same command-line interface
 * and semantics as upstream.
 */

#include <err.h>
#include <errno.h>
#include <search.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include "zfs_fletcher.h"
#include "zstream.h"
#include "zstream_shared.h"

#define	KEYSIZE	64

static void
make_key(char *key, uint64_t object, uint64_t offset)
{
	(void) snprintf(key, KEYSIZE, "%llu,%llu", (u_longlong_t)object,
	    (u_longlong_t)offset);
}

int
zstream_do_drop_records(int argc, char *argv[])
{
	int bufsz = SPA_MAXBLOCKSIZE;
	char *buf = safe_malloc(bufsz);
	dmu_replay_record_t thedrr;
	dmu_replay_record_t *drr = &thedrr;
	zio_cksum_t stream_cksum;
	FILE *infile = stdin;
	boolean_t verbose = B_FALSE;
	int c;

	while ((c = getopt(argc, argv, "v")) != -1) {
		switch (c) {
		case 'v':
			verbose = B_TRUE;
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

	/*
	 * Record specifiers are "object,offset". A trailing argument
	 * without a comma names the input file.
	 */
	int nspecs = 0;
	while (nspecs < argc && strchr(argv[nspecs], ',') != NULL)
		nspecs++;
	if (argc - nspecs > 1) {
		errx(1, "invalid record specifier '%s'", argv[nspecs]);
	} else if (argc - nspecs == 1) {
		infile = fopen(argv[nspecs], "r");
		if (infile == NULL)
			err(1, "invalid record specifier or input file '%s'",
			    argv[nspecs]);
	}

	if (hcreate(MAX(nspecs, 1) * 2) == 0)
		errx(1, "hcreate");
	for (int i = 0; i < nspecs; i++) {
		char *spec = argv[i];
		char *obj_str = strsep(&spec, ",");
		char *offset_str = strsep(&spec, ",");
		char *end;
		uint64_t object, offset;

		if (offset_str == NULL || spec != NULL)
			errx(1, "invalid record specifier '%s'", argv[i]);
		errno = 0;
		object = strtoull(obj_str, &end, 0);
		if (errno || *end != '\0')
			errx(1, "invalid value for object");
		offset = strtoull(offset_str, &end, 0);
		if (errno || *end != '\0')
			errx(1, "invalid value for offset");

		char *key = safe_malloc(KEYSIZE);
		make_key(key, object, offset);
		ENTRY e = {.key = key, .data = NULL};
		if (hsearch(e, ENTER) == NULL)
			errx(1, "hsearch");
	}

	if (infile == stdin && isatty(STDIN_FILENO)) {
		(void) fprintf(stderr,
		    "Error: The send stream is a binary format "
		    "and can not be read from a\n"
		    "terminal.  Standard input must be redirected.\n");
		exit(1);
	}

	zstream_libs_init();	/* illumos: libzpool must be up first */
	fletcher_4_init();
	int begin = 0;
	boolean_t seen = B_FALSE;
	while (sfread(drr, sizeof (*drr), infile) != 0) {
		uint64_t payload_size = 0;
		uint64_t object = 0, offset = 0;
		const char *record_type = NULL;

		switch (drr->drr_type) {
		case DRR_BEGIN:
		{
			ZIO_SET_CHECKSUM(&stream_cksum, 0, 0, 0, 0);
			VERIFY0(begin++);
			seen = B_TRUE;

			uint32_t sz = drr->drr_payloadlen;

			VERIFY3U(sz, <=, 1U << 28);

			if (sz != 0) {
				if (sz > bufsz) {
					buf = realloc(buf, sz);
					if (buf == NULL)
						err(1, "realloc");
					bufsz = sz;
				}
				(void) sfread(buf, sz, infile);
			}
			payload_size = sz;
			break;
		}
		case DRR_END:
		{
			struct drr_end *drre = &drr->drr_u.drr_end;
			/*
			 * Replication streams have an end of stream END
			 * record, so we cannot simply check --begin == 0.
			 */
			VERIFY3B(seen, ==, B_TRUE);
			begin--;
			/*
			 * Use the recalculated checksum, unless this is
			 * the END record of a stream package, which has
			 * no checksum.
			 */
			if (!ZIO_CHECKSUM_IS_ZERO(&drre->drr_checksum))
				drre->drr_checksum = stream_cksum;
			break;
		}
		case DRR_OBJECT:
		{
			struct drr_object *drro = &drr->drr_u.drr_object;
			VERIFY3S(begin, ==, 1);

			if (drro->drr_bonuslen > 0) {
				payload_size = DRR_OBJECT_PAYLOAD_SIZE(drro);
				(void) sfread(buf, payload_size, infile);
			}
			break;
		}
		case DRR_SPILL:
		{
			struct drr_spill *drrs = &drr->drr_u.drr_spill;
			VERIFY3S(begin, ==, 1);
			payload_size = DRR_SPILL_PAYLOAD_SIZE(drrs);
			(void) sfread(buf, payload_size, infile);
			break;
		}
		case DRR_WRITE_BYREF:
			VERIFY3S(begin, ==, 1);
			errx(1, "Deduplicated streams are not supported");
			break;
		case DRR_WRITE:
		{
			struct drr_write *drrw = &drr->drr_u.drr_write;
			VERIFY3S(begin, ==, 1);
			payload_size = DRR_WRITE_PAYLOAD_SIZE(drrw);
			(void) sfread(buf, payload_size, infile);
			object = drrw->drr_object;
			offset = drrw->drr_offset;
			record_type = "WRITE";
			break;
		}
		case DRR_WRITE_EMBEDDED:
		{
			struct drr_write_embedded *drrwe =
			    &drr->drr_u.drr_write_embedded;
			VERIFY3S(begin, ==, 1);
			payload_size =
			    P2ROUNDUP((uint64_t)drrwe->drr_psize, 8);
			(void) sfread(buf, payload_size, infile);
			object = drrwe->drr_object;
			offset = drrwe->drr_offset;
			record_type = "WRITE_EMBEDDED";
			break;
		}
		case DRR_FREEOBJECTS:
		case DRR_FREE:
		case DRR_OBJECT_RANGE:
			VERIFY3S(begin, ==, 1);
			break;
		default:
			errx(1, "invalid record type 0x%x", drr->drr_type);
		}

		if (record_type != NULL) {
			char key[KEYSIZE];
			make_key(key, object, offset);
			ENTRY e = {.key = key};
			if (hsearch(e, FIND) != NULL) {
				if (verbose) {
					warnx("dropping %s record for object "
					    "%llu offset %llu", record_type,
					    (u_longlong_t)object,
					    (u_longlong_t)offset);
				}
				continue;
			}
		}

		/*
		 * We need to recalculate the checksum, and it needs to be
		 * initially zero to do that.  BEGIN records don't have
		 * a checksum.
		 */
		if (drr->drr_type != DRR_BEGIN) {
			memset(&drr->drr_u.drr_checksum.drr_checksum, 0,
			    sizeof (drr->drr_u.drr_checksum.drr_checksum));
		}
		if (dump_record(drr, buf, payload_size,
		    &stream_cksum, STDOUT_FILENO) != 0)
			break;
		if (drr->drr_type == DRR_END) {
			/*
			 * A stream package ends with two END records; the
			 * last one's checksum starts from zero.
			 */
			ZIO_SET_CHECKSUM(&stream_cksum, 0, 0, 0, 0);
		}
	}
	if (ferror(infile))
		err(1, "error reading stream");
	if (infile != stdin)
		(void) fclose(infile);
	free(buf);
	fletcher_4_fini();
	hdestroy();

	return (0);
}
