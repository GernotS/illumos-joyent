/*
 * BSD 3-Clause New License (https://spdx.org/licenses/BSD-3-Clause.html)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2016-2018, Klara Inc.
 * Copyright (c) 2016-2018, Allan Jude
 * Copyright (c) 2018-2020, Sebastian Gottschall
 * Copyright (c) 2019-2020, Michael Niewöhner
 * Copyright (c) 2020, The FreeBSD Foundation [1]
 *
 * [1] Portions of this software were developed by Allan Jude
 *     under sponsorship from the FreeBSD Foundation.
 *
 * Copyright 2026 Gernot Strasser
 */

/*
 * On-disk header of a zstd-compressed ZFS block, shared with OpenZFS.
 * Both fields are stored big-endian.
 */

#ifndef	_SYS_ZSTD_ZSTD_H
#define	_SYS_ZSTD_ZSTD_H

#include <sys/types.h>
#include <sys/byteorder.h>
#include <sys/bitops.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct zfs_zstd_header {
	/* Compressed size of data */
	uint32_t c_len;

	/*
	 * Version and compression level
	 * We used to use a union to reference compression level
	 * and version easily, but as it turns out, relying on the
	 * ordering of bitfields is not remotely portable.
	 * So now we have get/set functions in zfs_zstd.c for
	 * manipulating this in just the right way forever.
	 */
	uint32_t raw_version_level;
	char data[];
} zfs_zstdhdr_t;

typedef struct zfs_zstd_meta {
	uint8_t level;
	uint32_t version;
} zfs_zstdmeta_t;

static inline void
zfs_get_hdrmeta(const zfs_zstdhdr_t *blob, zfs_zstdmeta_t *res)
{
	uint32_t raw = blob->raw_version_level;
	uint8_t findme = 0xff;
	int shift;
	for (shift = 0; shift < 4; shift++) {
		findme = BF32_GET(raw, 8*shift, 8);
		if (findme == 0)
			break;
	}
	switch (shift) {
	case 0:
		res->level = BF32_GET(raw, 24, 8);
		res->version = BSWAP_32(raw);
		res->version = BF32_GET(res->version, 8, 24);
		break;
	case 1:
		res->level = BF32_GET(raw, 0, 8);
		res->version = BSWAP_32(raw);
		res->version = BF32_GET(res->version, 0, 24);
		break;
	case 2:
		res->level = BF32_GET(raw, 24, 8);
		res->version = BF32_GET(raw, 0, 24);
		break;
	case 3:
		res->level = BF32_GET(raw, 0, 8);
		res->version = BF32_GET(raw, 8, 24);
		break;
	default:
		res->level = 0;
		res->version = 0;
		break;
	}
}

static inline uint8_t
zfs_get_hdrlevel(const zfs_zstdhdr_t *blob)
{
	uint8_t level = 0;
	zfs_zstdmeta_t res;
	zfs_get_hdrmeta(blob, &res);
	level = res.level;
	return (level);
}

static inline uint32_t
zfs_get_hdrversion(const zfs_zstdhdr_t *blob)
{
	uint32_t version = 0;
	zfs_zstdmeta_t res;
	zfs_get_hdrmeta(blob, &res);
	version = res.version;
	return (version);

}

static inline void
zfs_set_hdrversion(zfs_zstdhdr_t *blob, uint32_t version)
{
	BF32_SET(blob->raw_version_level, 0, 24, version);
}

static inline void
zfs_set_hdrlevel(zfs_zstdhdr_t *blob, uint8_t level)
{
	BF32_SET(blob->raw_version_level, 24, 8, level);
}


#ifdef	__cplusplus
}
#endif

#endif /* _SYS_ZSTD_ZSTD_H */
