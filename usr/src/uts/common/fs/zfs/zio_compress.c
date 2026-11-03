/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2013 by Saso Kiselkov. All rights reserved.
 * Copyright (c) 2013, 2018 by Delphix. All rights reserved.
 * Copyright (c) 2019, 2020, Klara Inc.
 * Copyright (c) 2019, Allan Jude
 * Copyright 2026 Gernot Strasser
 */

#include <sys/zfs_context.h>
#include <sys/compress.h>
#include <sys/spa.h>
#include <sys/zfeature.h>
#include <sys/zio.h>
#include <sys/zio_compress.h>

/*
 * If nonzero, every 1/X decompression attempts will fail, simulating
 * an undetected memory error.
 */
uint64_t zio_decompress_fail_fraction = 0;

/*
 * Compression vectors.
 */
zio_compress_info_t zio_compress_table[ZIO_COMPRESS_FUNCTIONS] = {
	{"inherit",		0,	NULL,		NULL},
	{"on",			0,	NULL,		NULL},
	{"uncompressed",	0,	NULL,		NULL},
	{"lzjb",		0,	lzjb_compress,	lzjb_decompress},
	{"empty",		0,	NULL,		NULL},
	{"gzip-1",		1,	gzip_compress,	gzip_decompress},
	{"gzip-2",		2,	gzip_compress,	gzip_decompress},
	{"gzip-3",		3,	gzip_compress,	gzip_decompress},
	{"gzip-4",		4,	gzip_compress,	gzip_decompress},
	{"gzip-5",		5,	gzip_compress,	gzip_decompress},
	{"gzip-6",		6,	gzip_compress,	gzip_decompress},
	{"gzip-7",		7,	gzip_compress,	gzip_decompress},
	{"gzip-8",		8,	gzip_compress,	gzip_decompress},
	{"gzip-9",		9,	gzip_compress,	gzip_decompress},
	{"zle",			64,	zle_compress,	zle_decompress},
	{"lz4",			0,	lz4_compress,	lz4_decompress},
	{"zstd",		ZIO_ZSTD_LEVEL_DEFAULT,
	    zfs_zstd_compress,	zfs_zstd_decompress}
};

uint8_t
zio_complevel_select(spa_t *spa, enum zio_compress compress, uint8_t child,
    uint8_t parent)
{
	uint8_t result;

	if (!ZIO_COMPRESS_HASLEVEL(compress))
		return (0);

	result = child;
	if (result == ZIO_COMPLEVEL_INHERIT)
		result = parent;

	return (result);
}

enum zio_compress
zio_compress_select(spa_t *spa, enum zio_compress child,
    enum zio_compress parent)
{
	enum zio_compress result;

	ASSERT(child < ZIO_COMPRESS_FUNCTIONS);
	ASSERT(parent < ZIO_COMPRESS_FUNCTIONS);
	ASSERT(parent != ZIO_COMPRESS_INHERIT);

	result = child;
	if (result == ZIO_COMPRESS_INHERIT)
		result = parent;

	if (result == ZIO_COMPRESS_ON) {
		if (spa_feature_is_active(spa, SPA_FEATURE_LZ4_COMPRESS))
			result = ZIO_COMPRESS_LZ4_ON_VALUE;
		else
			result = ZIO_COMPRESS_LEGACY_ON_VALUE;
	}

	return (result);
}

/*ARGSUSED*/
static int
zio_compress_zeroed_cb(void *data, size_t len, void *private)
{
	uint64_t *end = (uint64_t *)((char *)data + len);
	for (uint64_t *word = (uint64_t *)data; word < end; word++)
		if (*word != 0)
			return (1);

	return (0);
}

size_t
zio_compress_data(enum zio_compress c, abd_t *src, void *dst, size_t s_len,
    uint8_t level)
{
	size_t c_len, d_len;
	int complevel;
	zio_compress_info_t *ci = &zio_compress_table[c];

	ASSERT((uint_t)c < ZIO_COMPRESS_FUNCTIONS);
	ASSERT((uint_t)c == ZIO_COMPRESS_EMPTY || ci->ci_compress != NULL);

	/*
	 * If the data is all zeroes, we don't even need to allocate
	 * a block for it.  We indicate this by returning zero size.
	 */
	if (abd_iterate_func(src, 0, s_len, zio_compress_zeroed_cb, NULL) == 0)
		return (0);

	if (c == ZIO_COMPRESS_EMPTY)
		return (s_len);

	complevel = ci->ci_level;
	if (c == ZIO_COMPRESS_ZSTD) {
		/*
		 * The level is part of what is written (it is recorded in the
		 * block header and changes the compressed bytes), so a block
		 * whose level is unknown must not be compressed.
		 */
		if (level == ZIO_COMPLEVEL_INHERIT)
			return (s_len);

		if (level == ZIO_COMPLEVEL_DEFAULT)
			complevel = ZIO_ZSTD_LEVEL_DEFAULT;
		else
			complevel = level;
	}

	/* Compress at least 12.5% */
	d_len = s_len - (s_len >> 3);

	/* No compression algorithms can read from ABDs directly */
	void *tmp = abd_borrow_buf_copy(src, s_len);
	c_len = ci->ci_compress(tmp, dst, s_len, d_len, complevel);
	abd_return_buf(src, tmp, s_len);

	if (c_len > d_len)
		return (s_len);

	ASSERT3U(c_len, <=, d_len);
	return (c_len);
}

int
zio_decompress_data_buf(enum zio_compress c, void *src, void *dst,
    size_t s_len, size_t d_len)
{
	zio_compress_info_t *ci = &zio_compress_table[c];
	if ((uint_t)c >= ZIO_COMPRESS_FUNCTIONS || ci->ci_decompress == NULL)
		return (SET_ERROR(EINVAL));

	return (ci->ci_decompress(src, dst, s_len, d_len, ci->ci_level));
}

void *zio_decompress_failed_buf;

/*
 * Decompress, and if level is not NULL and the algorithm records one,
 * return the level the block was compressed with.
 */
int
zio_decompress_data_level(enum zio_compress c, abd_t *src, void *dst,
    size_t s_len, size_t d_len, uint8_t *level)
{
	void *tmp = abd_borrow_buf_copy(src, s_len);
	int ret;

	if (c == ZIO_COMPRESS_ZSTD && level != NULL)
		ret = zfs_zstd_decompress_level(tmp, dst, s_len, d_len, level);
	else
		ret = zio_decompress_data_buf(c, tmp, dst, s_len, d_len);

	/*
	 * Decompression shouldn't fail, because we've already verified
	 * the checksum.  However, for extra protection (e.g. against bitflips
	 * in non-ECC RAM), we handle this error (and test it).
	 */
	if (ret != 0) {
		zio_decompress_failed_buf = kmem_alloc(s_len, KM_SLEEP);
		bcopy(tmp, zio_decompress_failed_buf, s_len);
		panic("decompression failed "
		    "err=%u c=%u buf=%p s_len=%u d_len=%u",
		    ret, (int)c, zio_decompress_failed_buf,
		    (int)s_len, (int)d_len);
	}

	abd_return_buf(src, tmp, s_len);

	if (zio_decompress_fail_fraction != 0 &&
	    spa_get_random(zio_decompress_fail_fraction) == 0)
		ret = SET_ERROR(EINVAL);

	return (ret);
}

int
zio_decompress_data(enum zio_compress c, abd_t *src, void *dst,
    size_t s_len, size_t d_len)
{
	return (zio_decompress_data_level(c, src, dst, s_len, d_len, NULL));
}

/*
 * Return the per-dataset feature that must be active for blocks compressed
 * with the given algorithm to exist in a dataset, or SPA_FEATURE_NONE.
 */
int
zio_compress_to_feature(enum zio_compress comp)
{
	switch (comp) {
	case ZIO_COMPRESS_ZSTD:
		return (SPA_FEATURE_ZSTD_COMPRESS);
	default:
		break;
	}
	return (SPA_FEATURE_NONE);
}
