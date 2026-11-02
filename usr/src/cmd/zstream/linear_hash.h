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

#ifndef	_LINEAR_HASH_H
#define	_LINEAR_HASH_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Callers are required neither to manage iterators nor to pursue iterations
 * to completion. In return, callers must limit themselves to MAX_LH_ITERATORS
 * concurrent iterators.
 */
#define	MAX_LH_ITERATORS	8

struct linear_hash;
typedef struct linear_hash *linear_hash_t;

struct lh_iterator;
typedef struct lh_iterator *lh_iterator_t;

/*
 * Initialize a memory-based or convertible linear hash table.
 *
 * record_size	Size of each data record in bytes
 * max_memory	Maximum memory for storage
 * cache_dir	Directory in which to store convertible files
 * Returns a malloc'ed linear hash table or aborts.
 */
linear_hash_t
lh_init(size_t record_size, size_t max_memory, const char *cache_dir);

/*
 * Insert a data record with the given hash value.
 */
void
lh_insert(linear_hash_t lh, uint64_t hash, const void *data);

/*
 * Set up retrieval for all data records with a given hash value.
 * Use lh_retrieve_next() to iterate through matching records.
 */
lh_iterator_t
lh_initiate_retrieve(linear_hash_t lh, uint64_t hash);

/*
 * Get next data record matching the hash in the iterator. Returns B_TRUE if
 * buffer is valid, B_FALSE if no more records.
 */
boolean_t
lh_retrieve_next(lh_iterator_t iter, void *buffer);

/*
 * Destroy hash table and free all resources. lh may be NULL.
 */
void
lh_destroy(linear_hash_t lh);

#ifdef	__cplusplus
}
#endif

#endif /* _LINEAR_HASH_H */
