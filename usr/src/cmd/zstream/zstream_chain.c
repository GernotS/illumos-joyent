// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Garth Snyder. All rights reserved.
 */

/*
 * illumos port notes:
 *   - The userland ZFS subsystems (abd, zio, fletcher4, refcount, taskqs, ...)
 *     are brought up as a group by libzpool's kernel_init()/kernel_fini() -
 *     exactly as zdb and ztest do - rather than by OpenZFS's libspl_init() /
 *     fletcher_4_init() / abd_init() calls. libspl does not exist on illumos.
 *     We init once here in zstream_chain_exec() so every chain-based
 *     subcommand (dump, decompress, redup, dedup) has the subsystems up; the
 *     per-command libspl_init() call has been removed from zstream_dedup.c.
 *   - C forbids initializing a variable-length array (Type a[n] = {}), which
 *     the upstream code relied on. Chains are short, so we use fixed
 *     MAX_CHAIN_LENGTH arrays and verify the bound instead.
 *   - pthread_create()/pthread_join() are wrapped in VERIFY0(), not assert(),
 *     so they are not compiled out of non-DEBUG (release) builds.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/debug.h>
#include <sys/zfs_context.h>

#include "zstream_chain.h"
#include "zstream_shared.h"
#include "zstream_io.h"

#define	MAX_CHAIN_LENGTH	32

typedef struct chain_info {
	chain_step_t	*ci_chain;
	int		ci_num_steps;
	zstream_queue_t	*ci_queues;	/* Sparse */
	size_t		ci_item_size;
	chain_attrs_t	ci_attrs;
} *chain_info_t;

typedef struct {
	chain_info_t	wc_chain_info;
	int		wc_first;
	int		wc_last;
} worker_context_t;

typedef void *pthread_worker(void *);

static void *
zstream_chain_worker(worker_context_t *context);

static void
chain_exec_serialized(chain_info_t chain);

boolean_t serialize_chains = B_FALSE;

static void
libraries_init(void)
{
	zstream_libs_init();
}

static void
libraries_fini(void)
{
	zstream_libs_fini();
}

void
zstream_chain_exec(zstream_chain_t chain, chain_attrs_t attrs, int num_steps)
{
	zstream_queue_t		queues[MAX_CHAIN_LENGTH] = {0};
	size_t			max_size = 0;
	worker_context_t	contexts[MAX_CHAIN_LENGTH] = {0};
	pthread_t		worker_threads[MAX_CHAIN_LENGTH] = {0};
	int			num_workers = 0;

	/*
	 * Debug aid: setting ZSTREAM_SERIAL in the environment forces the
	 * chain to run single-threaded with no queues/forwarding, which
	 * isolates stage bugs from the threaded-queue machinery.
	 */
	if (getenv("ZSTREAM_SERIAL") != NULL)
		serialize_chains = B_TRUE;

	VERIFY(num_steps > 0);
	VERIFY(num_steps <= MAX_CHAIN_LENGTH);
	if (chain[num_steps - 1].cs_type == CS_PARALLEL ||
	    chain[0].cs_type == CS_PARALLEL) {
		(void) fprintf(stderr, "A zstream_chain cannot start or end "
		    "with a parallel step.");
		exit(1);
	}

	libraries_init();

	for (int i = 0; i < num_steps; i++) {
		if (chain[i].cs_out_size > max_size) {
			max_size = chain[i].cs_out_size;
		}
		if (i > 0 && chain[i].cs_in_size != chain[i - 1].cs_out_size) {
			(void) fprintf(stderr, "Warning: chain items %d and %d "
			    "have mismatched packet sizes\n", i - 1, i);
		}
	}

	struct chain_info chain_info = {
		.ci_chain = chain,
		.ci_num_steps = num_steps,
		.ci_queues = queues,
		.ci_item_size = max_size,
		.ci_attrs = attrs
	};

	/* Debug aid: ZSTREAM_DEBUG=1 reports chain geometry on stderr. */
	if (getenv("ZSTREAM_DEBUG") != NULL) {
		(void) fprintf(stderr, "[chain] num_steps=%d item_size=%lu "
		    "serial=%d\n", num_steps, (unsigned long)max_size,
		    serialize_chains);
		for (int i = 0; i < num_steps; i++) {
			(void) fprintf(stderr, "[chain]  step %d: type=%d "
			    "in=%lu out=%lu process=%p\n", i,
			    (int)chain[i].cs_type,
			    (unsigned long)chain[i].cs_in_size,
			    (unsigned long)chain[i].cs_out_size,
			    chain[i].cs_type == CS_SERIAL ?
			    (void *)chain[i].serial.css_process :
			    (void *)chain[i].parallel.csp_process);
		}
	}
	if (serialize_chains) {
		chain_exec_serialized(&chain_info);
		libraries_fini();
		return;
	}

	/* Create parallel queues */
	for (int i = 0; i < num_steps; i++) {
		if (chain[i].cs_type == CS_PARALLEL) {
			chain_step_t *ci = &chain[i];
			zq_params_t queue_params = {
				.qp_process = ci->parallel.csp_process,
				.qp_estimate_cost = ci->parallel.csp_cost,
				.qp_context = ci->parallel.csp_context,
				.qp_item_size = max_size,
				.qp_batch_budget =
				    ci->parallel.csp_batch_budget,
				.qp_queue_length =
				    ci->parallel.csp_queue_length
			};
			queues[i] = zstream_queue_create(&queue_params);
		}
	}

	/* Create worker context bundles and assign step ranges */
	int last = 0;
	for (int first = 0; first < num_steps; first = last) {
		contexts[num_workers].wc_chain_info = &chain_info;
		contexts[num_workers].wc_first = first;
		for (last = first + 1; last < num_steps &&
		    chain[last].cs_type != CS_PARALLEL; last++)
			;
		if (last >= num_steps) {
			contexts[num_workers].wc_last = last - 1;
		} else {
			contexts[num_workers].wc_last = last;
		}
		num_workers++;
	}

	/* Create and monitor threads */
	for (int i = 0; i < num_workers; i++) {
		char buff[32];
		VERIFY0(pthread_create(&worker_threads[i], NULL,
		    (pthread_worker *)zstream_chain_worker, &contexts[i]));
		(void) snprintf(buff, sizeof (buff), "chain-%d", i);
		(void) pthread_setname_np(worker_threads[i], buff);
	}
	for (int i = 0; i < num_workers; i++) {
		VERIFY0(pthread_join(worker_threads[i], NULL));
	}

	libraries_fini();
}

static void *
zstream_chain_worker(worker_context_t *context)
{
	chain_info_t ci = context->wc_chain_info;
	uint8_t buffer[ci->ci_item_size];
	boolean_t done = B_FALSE;

	while (!done) {
		for (int i = context->wc_first; i <= context->wc_last; i++) {
			chain_step_t *step = &ci->ci_chain[i];
			zstream_queue_t queue = ci->ci_queues[i];
			if (step->cs_type == CS_SERIAL) {
				done = !step->serial.css_process(
				    done ? NULL : buffer,
				    step->serial.css_context, ci->ci_attrs) ||
				    done;
			} else if (i == context->wc_first) {
				done = done ||
				    !zstream_dequeue(queue, buffer);
			} else if (done) {
				zstream_queue_fini(queue);
			} else {
				zstream_enqueue(queue, buffer);
			}
		}
	}
	return (NULL);
}

/*
 * Execute a chain linearly, without queues and without multithreading. For
 * debugging. This option is triggered by setting serialize_chains to B_TRUE.
 */
static void
chain_exec_serialized(chain_info_t ci)
{
	uint8_t buffer[ci->ci_item_size];
	boolean_t done = B_FALSE;

	while (!done) {
		for (int i = 0; i < ci->ci_num_steps; i++) {
			if (ci->ci_chain[i].cs_type == CS_SERIAL) {
				uint8_t *arg = done ? NULL : buffer;
				done = done ||
				    !ci->ci_chain[i].serial.css_process(arg,
				    ci->ci_chain[i].serial.css_context,
				    ci->ci_attrs);
			} else if (!done) {
				size_t cost =
				    ci->ci_chain[i].parallel.csp_cost(buffer,
				    ci->ci_chain[i].parallel.csp_context);
				if (cost > 0) {
					ci->ci_chain[i].parallel.csp_process(
					    buffer,
					    ci->ci_chain[i].parallel.
					    csp_context);
				}
			}
		}
	}
}
