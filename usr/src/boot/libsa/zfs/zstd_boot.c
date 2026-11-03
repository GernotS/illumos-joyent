/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Gernot Strasser
 */

/*
 * zstd decompression for the boot loader, so that it can read ZFS blocks
 * written with compression=zstd[-N|-fast-N].
 *
 * Only the decompressor is needed. Following upstream zstd's single-file
 * build template (build/single_file_libs/zstd-in.c), the decompression
 * sources from usr/src/common/zstd/lib are compiled here as one
 * translation unit, so no special compiler flags are required.
 */

#include <sys/types.h>

/*
 * libsa provides malloc() and friends only as macros around Malloc() etc.
 * (see stand.h), so supply zstd's allocation hooks directly instead of
 * letting zstd_deps.h map them onto <stdlib.h>.
 */
extern void *Malloc(size_t, const char *, int);
extern void *Calloc(size_t, size_t, const char *, int);
extern void Free(void *, const char *, int);

#define	ZSTD_DEPS_MALLOC
#define	ZSTD_malloc(s)		Malloc((s), NULL, 0)
#define	ZSTD_calloc(n, s)	Calloc((n), (s), NULL, 0)
#define	ZSTD_free(p)		Free((p), NULL, 0)

/* Configuration, as for the kernel build (see uts/intel/zfs/Makefile). */
#define	DEBUGLEVEL		0
#define	MEM_MODULE
#define	XXH_NAMESPACE		ZSTD_
#define	XXH_PRIVATE_API
#define	XXH_INLINE_ALL
#define	ZSTD_LEGACY_SUPPORT	0
#define	ZSTD_LIB_DEPRECATED	0
#define	ZSTD_NOBENCH
#define	ZSTD_TRACE		0
#define	ZSTD_DISABLE_ASM	1
#define	ZSTD_NO_INTRINSICS
#define	DYNAMIC_BMI2		0
#define	ZSTD_STATIC_LINKING_ONLY

#define	ZSTD_DEPS_NEED_MATH64
#include "common/zstd_deps.h"

#include "common/entropy_common.c"
#include "common/error_private.c"
#include "common/fse_decompress.c"
#include "common/zstd_common.c"

#include "decompress/huf_decompress.c"
#include "decompress/zstd_ddict.c"
#include "decompress/zstd_decompress.c"
#include "decompress/zstd_decompress_block.c"

/*
 * A zstd-compressed ZFS block starts with an 8-byte header (see
 * uts/common/fs/zfs/sys/zstd/zstd.h): the big-endian length of the
 * compressed data that follows, then the big-endian zstd version and
 * compression level. The data is a "magicless" zstd frame.
 */
#define	ZFS_ZSTD_HDRSIZE	8

int zfs_zstd_decompress(void *, void *, size_t, size_t, int);

int
zfs_zstd_decompress(void *s_start, void *d_start, size_t s_len,
    size_t d_len, int level)
{
	static ZSTD_DCtx *dctx;
	const uint8_t *hdr = s_start;
	uint32_t c_len;
	size_t result;

	(void) level;
	if (s_len < ZFS_ZSTD_HDRSIZE)
		return (-1);

	c_len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
	    ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
	if (c_len > s_len - ZFS_ZSTD_HDRSIZE)
		return (-1);

	/* The loader is single-threaded; keep one context for all reads. */
	if (dctx == NULL) {
		dctx = ZSTD_createDCtx();
		if (dctx == NULL)
			return (-1);
		if (ZSTD_isError(ZSTD_DCtx_setParameter(dctx, ZSTD_d_format,
		    ZSTD_f_zstd1_magicless))) {
			ZSTD_freeDCtx(dctx);
			dctx = NULL;
			return (-1);
		}
	}

	result = ZSTD_decompressDCtx(dctx, d_start, d_len,
	    hdr + ZFS_ZSTD_HDRSIZE, c_len);
	if (ZSTD_isError(result) || result != d_len)
		return (-1);

	return (0);
}
