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
 * illumos port: SHA-256 stands in for BLAKE3 as the dedup-table key hash.
 * See zstream_blake3.h for the rationale. SHA-256 produces a 32-byte digest,
 * exactly the size of a zio_cksum_t (4 x uint64_t). SHA2Init/Update/Final are
 * provided by libzpool (<sys/sha2.h>), the same routines ZFS uses for its
 * SHA-256 block checksums.
 */

#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/zfs_ioctl.h>
#include <sys/sha2.h>

#include "zstream_blake3.h"
#include "zstream_shared.h"

static void
chain_calc_blake3(drr_blake3_t *item, void *context)
{
	(void) context;
	drr_packet_t *base = &item->dp_base;
	assert(base->dp_payload_size > 0);

	SHA2_CTX ctx;
	SHA2Init(SHA256, &ctx);
	SHA2Update(&ctx, base->dp_payload, base->dp_payload_size);
	SHA2Final((uint8_t *)&item->dp_blake3_payload, &ctx);
}

chain_step_t
parallel_calc_blake3(void)
{
	chain_step_t step = {
		.cs_type = CS_PARALLEL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_blake3_t),
		.parallel = {
			.csp_queue_length = 256,
			.csp_batch_budget = 64 * 1024,
			.csp_process = (zq_process_item_f *)chain_calc_blake3,
			.csp_cost = (zq_estimate_cost_f *)payload_size_as_cost
		}
	};
	return (step);
}
