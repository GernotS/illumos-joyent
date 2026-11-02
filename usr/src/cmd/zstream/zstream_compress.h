// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the Common
 * Development and Distribution License ("CDDL"), version 1.0. You may only use
 * this file in accordance with the terms of version 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this source. A
 * copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

#ifndef	_ZSTREAM_COMPRESS_H
#define	_ZSTREAM_COMPRESS_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/zio_compress.h>
#include "zstream_chain.h"
#include "zstream_io.h"

/*
 * illumos port: illumos encodes the compression level in the algorithm
 * itself (gzip-1 ... gzip-9 are distinct enum zio_compress values) and has
 * no zstd, so the upstream cs_level field is not needed.
 */
typedef struct {
	enum zio_compress	cs_type;
} compression_spec_t;

/*
 * Decompress WRITE records, but only those that don't already match the
 * target compression type. Pass NULL to decompress unconditionally.
 */
chain_step_t
parallel_decompress_writes(compression_spec_t *target);

/*
 * Compress uncompressed WRITE records with the given compression type.
 * A target of ZIO_COMPRESS_OFF is a no-op.
 */
chain_step_t
parallel_compress_writes(compression_spec_t target);

#ifdef	__cplusplus
}
#endif

#endif  /* _ZSTREAM_COMPRESS_H */
