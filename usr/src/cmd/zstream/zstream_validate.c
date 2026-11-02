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

#include <stdio.h>
#include <sys/types.h>
#include <sys/zfs_ioctl.h>

#include "zstream.h"
#include "zstream_validate.h"
#include "zstream_io.h"
#include "zstream_shared.h"

/*
 * This is currently just a skeleton into which validation scraps from
 * elsewhere can be integrated. Overall, it looks like there may not be enough
 * validation going on in general to make this module necessary.
 */
/*
 * illumos port note - removed the "nesting" assertions.
 *
 * Upstream tracked stream nesting in an int local to this function, reset to
 * 0 on every call, then asserted VERIFY3U(nesting, ==, 1) for every non-BEGIN
 * record. That can never hold: the first DRR_OBJECT aborts the process
 * (assfail3 from a queue_worker thread). Promoting it to a static would not
 * fix it either - this is a CS_PARALLEL step, so records arrive concurrently
 * and out of order, and a shared counter would both race and be meaningless.
 *
 * Nesting is a property of the record *sequence*, so it can only be checked
 * in a serial stage. Until such a check is wanted, this step validates only
 * what is decidable from a single record in isolation. Upstream describes
 * this module as "just a skeleton", so nothing of substance is lost.
 */
static void
chain_validate_records(drr_packet_t *item, void *context)
{
	(void) context;

	if (!item) {
		return;
	}

	struct dmu_replay_record *drr	 = &item->dp_drr;
	struct drr_write *drrw		 = &drr->drr_u.drr_write;

	if (!item->dp_stream_offset && drr->drr_type != DRR_BEGIN) {
		fprintf(stderr, "Warning: first record is not DRR_BEGIN\n");
	}

	switch (drr->drr_type) {
	case DRR_BEGIN:
		VERIFY3U(item->dp_payload_size, <=, 1UL << 28);
		break;

	case DRR_WRITE:
		if (drrw->drr_compressiontype >= ZIO_COMPRESS_FUNCTIONS) {
			fprintf(stderr, "Invalid compression type: %d\n",
			    drrw->drr_compressiontype);
			exit(3);
		}
		break;

	case DRR_END:
	case DRR_OBJECT:
	case DRR_SPILL:
	case DRR_WRITE_BYREF:
	case DRR_WRITE_EMBEDDED:
	case DRR_FREEOBJECTS:
	case DRR_FREE:
	case DRR_OBJECT_RANGE:
		break;

	default:
		fprintf(stderr, "Unknown record type: %d\n", drr->drr_type);
		exit(1);
	}
}

chain_step_t
parallel_validate_records(void)
{
	/*
	 * Upstream bug (GarthSnyder/zfs zstream-dedup-forwarding): this step
	 * declared .cs_type = CS_SERIAL while initializing the .parallel arm of
	 * the union. The chain then dispatched it as a serial step and called
	 * .serial.css_process, which occupies the same union offset as
	 * .parallel.csp_queue_length - so it jumped to the integer 64 (0x40)
	 * and died. It must be CS_PARALLEL to match the initializer.
	 */
	chain_step_t step = {
		.cs_type = CS_PARALLEL,
		.cs_in_size = sizeof (drr_packet_t),
		.cs_out_size = sizeof (drr_packet_t),
		.parallel = {
			.csp_queue_length = 64,
			.csp_batch_budget = 128 * 1024,
			.csp_process =
			    (zq_process_item_f *)chain_validate_records,
			.csp_cost = (zq_estimate_cost_f *)constant_cost_of_one
		}
	};
	return (step);
}
