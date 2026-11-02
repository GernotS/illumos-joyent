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

/*
 * illumos port: upstream keys the dedup table on a BLAKE3 payload digest.
 * BLAKE3 is not present in illumos-gate, so this stage computes a SHA-256
 * digest instead. The hash is used only as an in-memory dedup-table key for
 * the duration of a single "zstream dedup" run (candidates are always
 * confirmed by a full record comparison in writes_compatible()), so the
 * particular algorithm need not match anything on disk or any other
 * implementation. Both digests are 256 bits and fit a zio_cksum_t, so the
 * surrounding pipeline and the drr_blake3_t/dp_blake3_payload field names are
 * retained unchanged.
 */

#ifndef	_ZSTREAM_BLAKE3_H
#define	_ZSTREAM_BLAKE3_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/zfs_ioctl.h>
#include "zstream_io.h"

typedef struct {
	drr_packet_t	dp_base;
	zio_cksum_t	dp_blake3_payload;	/* SHA-256 digest on illumos */
} drr_blake3_t;

chain_step_t
parallel_calc_blake3(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _ZSTREAM_BLAKE3_H */
