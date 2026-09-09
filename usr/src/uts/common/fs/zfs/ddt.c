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
 * Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2016 by Delphix. All rights reserved.
 * Copyright (c) 2022 by Pawel Jakub Dawidek
 * Portions Copyright (c) 2024, Klara, Inc.
 * Portions Copyright (c) 2024, iXsystems, Inc.
 * Copyright 2026 Gernot Strasser
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/ddt.h>
#include <sys/ddt_impl.h>
#include <sys/zap.h>
#include <sys/dnode.h>
#include <sys/dmu_tx.h>
#include <sys/arc.h>
#include <sys/dsl_pool.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_synctask.h>
#include <sys/abd.h>
#include <sys/zfeature.h>

/* DEBUG-ONLY: file-based DDT trace (zfs_dbgmsg ring is too small for ztest) */
#ifndef _KERNEL
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
static void
ddt_dbg(const char *fmt, ...)
{
	static FILE *fp = NULL;
	static kmutex_t lk;
	static boolean_t lk_initted = B_FALSE;
	if (!lk_initted) {
		mutex_init(&lk, NULL, MUTEX_DEFAULT, NULL);
		lk_initted = B_TRUE;
	}
	mutex_enter(&lk);
	if (fp == NULL) {
		char path[64];
		(void) snprintf(path, sizeof (path),
		    "/tmp/ddt-trace.%d.log", (int)getpid());
		fp = fopen(path, "a");
	}
	if (fp != NULL) {
		va_list ap;
		va_start(ap, fmt);
		(void) vfprintf(fp, fmt, ap);
		va_end(ap);
		(void) fputc('\n', fp);
		(void) fflush(fp);
	}
	mutex_exit(&lk);
}
#else
#define	ddt_dbg(...)	/* kernel: no-op */
#endif


/*
 * Enable/disable prefetching of dedup-ed blocks which are going to be freed.
 */
int zfs_dedup_prefetch = 1;

/*
 * How many DDT prune entries to add to the DDT sync AVL tree per txg when
 * processing a "zpool ddtprune".  Caps the per-txg work so pruning a large
 * table doesn't stall sync.  Ported from OpenZFS PR #16277.
 */
static uint32_t zfs_ddt_prunes_per_txg = 50000;

/*
 * Whether "zpool ddtprune" prefetches the unique-class DDT store objects
 * into the ARC before walking them.  The walk uses a non-prefetching ZAP
 * cursor (see ddt_zap_walk(); appropriate for scrub), so without this every
 * cold ZAP leaf is a synchronous random read and the walk is I/O-bound.
 * Issuing a whole-object async prefetch first overlaps those reads and makes
 * the walk ARC-warm.  Equivalent to running "zpool prefetch -t ddt" by hand.
 * Set to B_FALSE if the DDT is much larger than the ARC, where prefetching
 * only causes eviction churn.
 */
static boolean_t zfs_ddt_prune_prefetch = B_TRUE;

/*
 * Debug-only knob: when set non-zero, ddt_class_start() subtracts a random
 * delta from the current wall-clock time so fresh DDT entries appear to have
 * a spread of ages.  Lets `zpool ddtprune -p N` exercise its percentile
 * cutoff math against synthetic entries without waiting weeks for real
 * aging.  Declared volatile so the compiler does not dead-eliminate the read
 * when always initialised to B_FALSE.  Set/clear via mdb -kw, e.g.:
 *
 *     echo "ddt_prune_artificial_age/W 0t1" | mdb -kw
 */
volatile boolean_t ddt_prune_artificial_age = B_FALSE;

/*
 * Debug-only knob: when set non-zero, ddt_sync_entry() and
 * ddt_sync_flush_entry() deliberately hand ddt_object_remove() a class the
 * entry is NOT stored in, simulating the in-core/on-disk class drift behind
 * openzfs/zfs#17528.  Used by the dedup_ddt_stray_class ZTS test to exercise
 * the ddt_object_remove_stray() recovery, which has no known natural trigger.
 * Declared volatile for the same reason as ddt_prune_artificial_age.  Set and
 * cleared via mdb -kw, e.g.:
 *
 *     echo "ddt_inject_stray_class/W 0t1" | mdb -kw
 *
 * Never set this on a pool holding data you care about: it is a deliberate
 * corruption of the sync path.
 */
volatile boolean_t ddt_inject_stray_class = B_FALSE;

/*
 * Count of stray-class recoveries performed by ddt_object_remove_stray().
 *
 * Two jobs.  In the field it is the only externally visible sign that
 * class/type drift is happening at all -- the recovery is otherwise
 * silent apart from a zfs_dbgmsg, and a non-zero value here is evidence
 * worth attaching to openzfs/zfs#17528.  In the test suite it is what
 * makes dedup_ddt_stray_class meaningful: without it the test passes
 * whenever the pool survives, including when the injector never fired
 * and nothing was exercised at all.
 *
 * Read with: mdb -k -e "ddt_stray_recoveries/E"
 */
uint64_t ddt_stray_recoveries = 0;

/*
 * Pick a class that is NOT the one the entry is really in, for the injector
 * above.  Rotating through the classes keeps the recovery scan honest -- it
 * must find the entry wherever it actually lives, not in a fixed place.
 */
static ddt_class_t
ddt_stray_class(ddt_class_t real)
{
	return ((real + 1) % DDT_CLASSES);
}

/*
 * Records the wall-clock time an entry first enters the UNIQUE class; read by
 * ddt_prune_walk() to decide which entries are old enough to prune.  Normally
 * just the current time; when ddt_prune_artificial_age is set, subtracts a
 * random delta (biased toward smaller subtracts) so prune-by-percentage has
 * age variance to bucket against in the histogram pass.
 */
static uint64_t
ddt_class_start(void)
{
	uint64_t start = gethrestime_sec();

	if (unlikely(ddt_prune_artificial_age)) {
		/*
		 * Random subtract of up to ~24 days, biased so the
		 * distribution isn't uniform.  Cheap PRNG is fine here --
		 * this is debug-only.
		 */
		uint64_t range = (1ULL << 21);	/* ~24 days in seconds */
		uint64_t roll;
		(void) random_get_pseudo_bytes((uint8_t *)&roll,
		    sizeof (roll));
		uint_t pct = (uint_t)(roll % 100);
		if (pct < 50)
			range >>= 4;
		else if (pct > 75)
			range >>= 1;
		start -= (roll % range);
	}

	return (start);
}

/*
 * If the dedup class cannot satisfy a DDT allocation, treat as over quota
 * for this many TXGs.
 */
uint_t dedup_class_wait_txgs = 5;

static const ddt_ops_t *ddt_ops[DDT_TYPES] = {
	&ddt_zap_ops,
};

static const char *ddt_class_name[DDT_CLASSES] = {
	"ditto",
	"duplicate",
	"unique",
};

static void
ddt_object_create(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    dmu_tx_t *tx)
{
	spa_t *spa = ddt->ddt_spa;
	objset_t *os = ddt->ddt_os;
	uint64_t *objectp = &ddt->ddt_object[type][class];
	boolean_t prehash = zio_checksum_table[ddt->ddt_checksum].ci_flags &
	    ZCHECKSUM_FLAG_DEDUP;
	char name[DDT_NAMELEN];

	ddt_object_name(ddt, type, class, name);

	ASSERT(*objectp == 0);
	VERIFY(ddt_ops[type]->ddt_op_create(os, objectp, tx, prehash) == 0);
	ASSERT(*objectp != 0);

	/* Cache a held dnode for the new store object (ff5414406). */
	VERIFY0(dnode_hold(os, *objectp, ddt,
	    &ddt->ddt_object_dnode[type][class]));

	/*
	 * Object link lives in dir_object (FDT) or the MOS root (legacy).
	 * Histogram always lives in the shared spa_ddt_stat_object; names
	 * carry the checksum prefix so they are pool-wide unique.
	 */
	uint64_t parent = (ddt->ddt_version == DDT_VERSION_FDT) ?
	    ddt->ddt_dir_object : DMU_POOL_DIRECTORY_OBJECT;
	ASSERT3U(parent, !=, 0);

	VERIFY(zap_add(os, parent, name,
	    sizeof (uint64_t), 1, objectp, tx) == 0);

	VERIFY(zap_add(os, spa->spa_ddt_stat_object, name,
	    sizeof (uint64_t), sizeof (ddt_histogram_t) / sizeof (uint64_t),
	    &ddt->ddt_histogram[type][class], tx) == 0);

	ddt_dbg("ddt_create: cksum=%u parent=%llu obj=%llu name=%s "
	    "ver=%llu flags=%llx txg=%llu",
	    (uint_t)ddt->ddt_checksum,
	    (u_longlong_t)parent, (u_longlong_t)*objectp, name,
	    (u_longlong_t)ddt->ddt_version,
	    (u_longlong_t)ddt->ddt_flags,
	    (u_longlong_t)dmu_tx_get_txg(tx));
}

static void
ddt_object_destroy(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    dmu_tx_t *tx)
{
	spa_t *spa = ddt->ddt_spa;
	objset_t *os = ddt->ddt_os;
	uint64_t *objectp = &ddt->ddt_object[type][class];
	char name[DDT_NAMELEN];

	ddt_object_name(ddt, type, class, name);

	ASSERT(*objectp != 0);
	{
		uint64_t count;
		VERIFY0(ddt_object_count(ddt, type, class, &count));
		ASSERT0(count);
	}
	ASSERT(ddt_histogram_empty(&ddt->ddt_histogram[type][class]));
	uint64_t parent = (ddt->ddt_version == DDT_VERSION_FDT) ?
	    ddt->ddt_dir_object : DMU_POOL_DIRECTORY_OBJECT;
	ASSERT3U(parent, !=, 0);
	uint64_t freed_obj = *objectp;

	/*
	 * Invalidate the object id first, under the writer lock, so that
	 * open-context readers (prefetch, walk, count, repair lookup)
	 * either see the object while it still exists or not at all.
	 * Ported from upstream 765929cb4.
	 */
	dnode_t *freed_dn = ddt->ddt_object_dnode[type][class];
	rw_enter(&ddt->ddt_objects_lock, RW_WRITER);
	*objectp = 0;
	ddt->ddt_object_dnode[type][class] = NULL;
	rw_exit(&ddt->ddt_objects_lock);

	if (freed_dn != NULL)
		dnode_rele(freed_dn, ddt);

	VERIFY(zap_remove(os, parent, name, tx) == 0);
	VERIFY(zap_remove(os, spa->spa_ddt_stat_object, name, tx) == 0);
	VERIFY(ddt_ops[type]->ddt_op_destroy(os, freed_obj, tx) == 0);
	bzero(&ddt->ddt_object_stats[type][class], sizeof (ddt_object_t));

	ddt_dbg("ddt_destroy: cksum=%u parent=%llu obj=%llu name=%s "
	    "ver=%llu flags=%llx txg=%llu",
	    (uint_t)ddt->ddt_checksum,
	    (u_longlong_t)parent, (u_longlong_t)freed_obj, name,
	    (u_longlong_t)ddt->ddt_version,
	    (u_longlong_t)ddt->ddt_flags,
	    (u_longlong_t)dmu_tx_get_txg(tx));
}

static int
ddt_object_load(ddt_t *ddt, ddt_type_t type, ddt_class_t class)
{
	ddt_object_t *ddo = &ddt->ddt_object_stats[type][class];
	dmu_object_info_t doi;
	char name[DDT_NAMELEN];
	int error;

	ddt_object_name(ddt, type, class, name);

	uint64_t parent = (ddt->ddt_version == DDT_VERSION_FDT) ?
	    ddt->ddt_dir_object : DMU_POOL_DIRECTORY_OBJECT;
	if (parent == 0)
		return (SET_ERROR(ENOENT));
	error = zap_lookup(ddt->ddt_os, parent, name,
	    sizeof (uint64_t), 1, &ddt->ddt_object[type][class]);

	if (error != 0)
		return (error);

	/* Cache a held dnode for the loaded store object (ff5414406). */
	error = dnode_hold(ddt->ddt_os, ddt->ddt_object[type][class], ddt,
	    &ddt->ddt_object_dnode[type][class]);
	if (error != 0)
		return (error);

	VERIFY0(zap_lookup(ddt->ddt_os, ddt->ddt_spa->spa_ddt_stat_object, name,
	    sizeof (uint64_t), sizeof (ddt_histogram_t) / sizeof (uint64_t),
	    &ddt->ddt_histogram[type][class]));

	/*
	 * Seed the cached statistics.
	 */
	VERIFY(ddt_object_info(ddt, type, class, &doi) == 0);

	VERIFY0(ddt_object_count(ddt, type, class, &ddo->ddo_count));
	ddo->ddo_dspace = doi.doi_physical_blocks_512 << 9;
	ddo->ddo_mspace = doi.doi_fill_count * doi.doi_data_block_size;

	return (0);
}

static void
ddt_object_sync(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    dmu_tx_t *tx)
{
	ddt_object_t *ddo = &ddt->ddt_object_stats[type][class];
	dmu_object_info_t doi;
	char name[DDT_NAMELEN];

	ddt_object_name(ddt, type, class, name);

	VERIFY(zap_update(ddt->ddt_os, ddt->ddt_spa->spa_ddt_stat_object, name,
	    sizeof (uint64_t), sizeof (ddt_histogram_t) / sizeof (uint64_t),
	    &ddt->ddt_histogram[type][class], tx) == 0);

	/*
	 * Cache DDT statistics; this is the only time they'll change.
	 */
	VERIFY(ddt_object_info(ddt, type, class, &doi) == 0);

	VERIFY0(ddt_object_count(ddt, type, class, &ddo->ddo_count));
	ddo->ddo_dspace = doi.doi_physical_blocks_512 << 9;
	ddo->ddo_mspace = doi.doi_fill_count * doi.doi_data_block_size;
}

static int
ddt_object_lookup(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    ddt_entry_t *dde)
{
	if (!ddt_object_exists(ddt, type, class))
		return (SET_ERROR(ENOENT));

	/* Ensure the back-pointer is set for stack-allocated entries. */
	dde->dde_ddt = ddt;
	return (ddt_ops[type]->ddt_op_lookup(
	    ddt->ddt_object_dnode[type][class], &dde->dde_key, dde->dde_phys,
	    DDT_PHYS_SIZE(ddt)));
}

/*
 * Like ddt_object_lookup(), but for open context where we need protection
 * against concurrent object destruction by sync context.
 */
static int
ddt_object_lookup_open(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    ddt_entry_t *dde)
{
	rw_enter(&ddt->ddt_objects_lock, RW_READER);
	int error = ddt_object_lookup(ddt, type, class, dde);
	rw_exit(&ddt->ddt_objects_lock);
	return (error);
}

static int
ddt_object_contains(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    const ddt_key_t *ddk)
{
	if (!ddt_object_exists(ddt, type, class))
		return (SET_ERROR(ENOENT));

	return (ddt_ops[type]->ddt_op_contains(
	    ddt->ddt_object_dnode[type][class], ddk));
}

static void
ddt_object_prefetch(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    ddt_entry_t *dde)
{
	/*
	 * Called from open context, so protect against concurrent
	 * object destruction by sync context.
	 */
	rw_enter(&ddt->ddt_objects_lock, RW_READER);

	if (ddt_object_exists(ddt, type, class)) {
		dde->dde_ddt = ddt;
		ddt_ops[type]->ddt_op_prefetch(
		    ddt->ddt_object_dnode[type][class], dde);
	}

	rw_exit(&ddt->ddt_objects_lock);
}

static void
ddt_object_prefetch_all(ddt_t *ddt, ddt_type_t type, ddt_class_t class)
{
	/*
	 * Called from open context, so protect against concurrent
	 * object destruction by sync context.
	 */
	rw_enter(&ddt->ddt_objects_lock, RW_READER);

	if (ddt_object_exists(ddt, type, class))
		ddt_ops[type]->ddt_op_prefetch_all(
		    ddt->ddt_object_dnode[type][class]);

	rw_exit(&ddt->ddt_objects_lock);
}

int
ddt_object_update(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    const ddt_lightweight_entry_t *ddlwe, dmu_tx_t *tx)
{
	ASSERT(ddt_object_exists(ddt, type, class));

	return (ddt_ops[type]->ddt_op_update(
	    ddt->ddt_object_dnode[type][class], &ddlwe->ddlwe_key,
	    &ddlwe->ddlwe_phys, DDT_PHYS_SIZE(ddt), tx));
}

static int
ddt_object_remove(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    const ddt_key_t *ddk, dmu_tx_t *tx)
{
	ASSERT(ddt_object_exists(ddt, type, class));

	return (ddt_ops[type]->ddt_op_remove(
	    ddt->ddt_object_dnode[type][class], ddk, tx));
}

/*
 * Log a stray-class recovery.
 *
 * Includes the entry key.  Without it every recovery for the same
 * checksum/type/class prints an identical line, so a burst is
 * ambiguous: N distinct entries drifting and one entry looping N times
 * look exactly the same, and those need very different responses.
 *
 * Capped, because a real drift event on a large DDT could produce one
 * message per entry and push everything else out of the dbgmsg ring --
 * destroying the context needed to work out what caused it.  The total
 * is always available in ddt_stray_recoveries.
 *
 * found_type == DDT_TYPES means the entry was not found anywhere.
 */
#define	DDT_STRAY_LOG_MAX	32

static void
ddt_stray_log(ddt_t *ddt, const ddt_key_t *ddk, ddt_type_t found_type,
    ddt_class_t found_class, ddt_type_t want_type, ddt_class_t want_class,
    int error)
{
	uint64_t n = ddt_stray_recoveries;

	if (n > DDT_STRAY_LOG_MAX) {
		if (n == DDT_STRAY_LOG_MAX + 1) {
			zfs_dbgmsg("ddt_object_remove: %s: further "
			    "stray-class messages suppressed; running total "
			    "is in ddt_stray_recoveries",
			    spa_name(ddt->ddt_spa));
		}
		return;
	}

	if (found_type == DDT_TYPES) {
		zfs_dbgmsg("ddt_object_remove: %s: entry %llx:%llx "
		    "(checksum %u) absent from all objects; expected "
		    "type %u class %u (openzfs#17528)",
		    spa_name(ddt->ddt_spa),
		    (u_longlong_t)ddk->ddk_cksum.zc_word[0],
		    (u_longlong_t)ddk->ddk_prop,
		    (uint_t)ddt->ddt_checksum,
		    (uint_t)want_type, (uint_t)want_class);
	} else {
		zfs_dbgmsg("ddt_object_remove: %s: entry %llx:%llx "
		    "(checksum %u) found in type %u class %u, not the "
		    "expected type %u class %u (error %d)",
		    spa_name(ddt->ddt_spa),
		    (u_longlong_t)ddk->ddk_cksum.zc_word[0],
		    (u_longlong_t)ddk->ddk_prop,
		    (uint_t)ddt->ddt_checksum,
		    (uint_t)found_type, (uint_t)found_class,
		    (uint_t)want_type, (uint_t)want_class, error);
	}
}

/*
 * Remove an entry whose in-core (type, class) turned out to be wrong.
 *
 * Called only after ddt_object_remove() has already returned ENOENT for
 * the nominal location, which means dde_type/dde_class disagree with the
 * on-disk layout.  Search the remaining objects and remove the entry from
 * whichever one actually holds it, so we do not leave a stray behind.
 *
 * Returns 0 if the entry was removed or is genuinely absent everywhere
 * (both are acceptable end states -- we were deleting it), or the error
 * from ddt_object_remove() if the real removal failed.
 *
 * ddt_object_contains() is the probe rather than ddt_object_remove()
 * because it checks ddt_object_exists() first; ddt_object_remove() would
 * ASSERT, then dereference a NULL dnode on a non-DEBUG kernel.
 */
static int
ddt_object_remove_stray(ddt_t *ddt, ddt_type_t skiptype,
    ddt_class_t skipclass, const ddt_key_t *ddk, dmu_tx_t *tx)
{
	for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
		for (ddt_class_t class = 0; class < DDT_CLASSES; class++) {
			int error;

			if (type == skiptype && class == skipclass)
				continue;
			if (!ddt_object_exists(ddt, type, class))
				continue;
			if (ddt_object_contains(ddt, type, class, ddk) != 0)
				continue;

			error = ddt_object_remove(ddt, type, class, ddk, tx);
			atomic_inc_64(&ddt_stray_recoveries);
			ddt_stray_log(ddt, ddk, type, class, skiptype,
			    skipclass, error);

			return (error);
		}
	}

	/*
	 * Not present anywhere.  We were deleting it, so this is the
	 * state we wanted; say so and carry on rather than taking the
	 * pool down.
	 */
	atomic_inc_64(&ddt_stray_recoveries);
	ddt_stray_log(ddt, ddk, DDT_TYPES, 0, skiptype, skipclass, 0);

	return (0);
}

/*
 * ddt_object_remove() for the sync path: tolerate an entry that is not
 * where we thought it was, rather than panicking the machine in
 * spa_sync() context and taking the pool down on every subsequent
 * import.  See openzfs/zfs#17528.
 */
static int
ddt_object_remove_sync(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    const ddt_key_t *ddk, dmu_tx_t *tx)
{
	int error;

	/*
	 * If the object itself is gone, drift is severe enough that the
	 * nominal location cannot even be probed: ddt_object_remove()
	 * ASSERTs on ddt_object_exists() and would then dereference a
	 * NULL dnode on a non-DEBUG kernel.  Go straight to the search.
	 */
	if (!ddt_object_exists(ddt, type, class))
		return (ddt_object_remove_stray(ddt, type, class, ddk, tx));

	error = ddt_object_remove(ddt, type, class, ddk, tx);

	if (error == ENOENT)
		error = ddt_object_remove_stray(ddt, type, class, ddk, tx);

	return (error);
}

int
ddt_object_walk(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    uint64_t *walk, ddt_entry_t *dde)
{
	/*
	 * Can be called from open context (ddt_prune_walk), so protect
	 * against concurrent object destruction by sync context.
	 */
	rw_enter(&ddt->ddt_objects_lock, RW_READER);

	if (!ddt_object_exists(ddt, type, class)) {
		rw_exit(&ddt->ddt_objects_lock);
		return (SET_ERROR(ENOENT));
	}

	dde->dde_ddt = ddt;
	int error = ddt_ops[type]->ddt_op_walk(
	    ddt->ddt_object_dnode[type][class], dde, walk);

	rw_exit(&ddt->ddt_objects_lock);
	return (error);
}

int
ddt_object_count(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    uint64_t *count)
{
	/*
	 * Can be called from open context (dedup table stats), so protect
	 * against concurrent object destruction by sync context.
	 */
	rw_enter(&ddt->ddt_objects_lock, RW_READER);

	if (!ddt_object_exists(ddt, type, class)) {
		rw_exit(&ddt->ddt_objects_lock);
		return (SET_ERROR(ENOENT));
	}

	int error = ddt_ops[type]->ddt_op_count(
	    ddt->ddt_object_dnode[type][class], count);

	rw_exit(&ddt->ddt_objects_lock);
	return (error);
}

int
ddt_object_info(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    dmu_object_info_t *doi)
{
	if (!ddt_object_exists(ddt, type, class))
		return (SET_ERROR(ENOENT));

	return (dmu_object_info(ddt->ddt_os, ddt->ddt_object[type][class],
	    doi));
}

boolean_t
ddt_object_exists(ddt_t *ddt, ddt_type_t type, ddt_class_t class)
{
	return (ddt->ddt_object_dnode[type][class] != NULL);
}

void
ddt_object_name(ddt_t *ddt, ddt_type_t type, ddt_class_t class,
    char *name)
{
	/*
	 * The DMU_POOL_DDT format ("DDT-<cksum>-<type>-<class>") is used
	 * for both layouts.  For FDT pools the name is redundant inside
	 * the checksum-scoped dir_object, but using one format means the
	 * shared spa_ddt_stat_object never sees name collisions across
	 * checksums.
	 */
	(void) snprintf(name, DDT_NAMELEN, DMU_POOL_DDT,
	    zio_checksum_table[ddt->ddt_checksum].ci_name,
	    ddt_ops[type]->ddt_op_name, ddt_class_name[class]);
}

void
ddt_bp_fill(const ddt_univ_phys_t *ddp, ddt_phys_variant_t v, blkptr_t *bp,
    uint64_t txg)
{
	ASSERT(txg != 0);
	ASSERT3U(v, !=, DDT_PHYS_NONE);

	const dva_t *dva;
	uint64_t birth;
	if (v == DDT_PHYS_FLAT) {
		dva = ddp->ddp_flat.ddp_dva;
		birth = ddp->ddp_flat.ddp_phys_birth;
	} else {
		dva = ddp->ddp_trad[v].ddp_dva;
		birth = ddp->ddp_trad[v].ddp_phys_birth;
	}
	for (int d = 0; d < SPA_DVAS_PER_BP; d++)
		bp->blk_dva[d] = dva[d];
	BP_SET_BIRTH(bp, txg, birth);
}

/*
 * The bp created via this function may be used for repairs and scrub, but it
 * will be missing the salt / IV required to do a full decrypting read.
 */
void
ddt_bp_create(enum zio_checksum checksum,
    const ddt_key_t *ddk, const ddt_univ_phys_t *ddp, ddt_phys_variant_t v,
    blkptr_t *bp)
{
	BP_ZERO(bp);

	if (ddp != NULL) {
		ASSERT3U(v, !=, DDT_PHYS_NONE);
		uint64_t birth = (v == DDT_PHYS_FLAT) ?
		    ddp->ddp_flat.ddp_phys_birth :
		    ddp->ddp_trad[v].ddp_phys_birth;
		ddt_bp_fill(ddp, v, bp, birth);
	}

	bp->blk_cksum = ddk->ddk_cksum;

	BP_SET_LSIZE(bp, DDK_GET_LSIZE(ddk));
	BP_SET_PSIZE(bp, DDK_GET_PSIZE(ddk));
	BP_SET_COMPRESS(bp, DDK_GET_COMPRESS(ddk));
	BP_SET_CRYPT(bp, DDK_GET_CRYPT(ddk));
	BP_SET_FILL(bp, 1);
	BP_SET_CHECKSUM(bp, checksum);
	BP_SET_TYPE(bp, DMU_OT_DEDUP);
	BP_SET_LEVEL(bp, 0);
	BP_SET_DEDUP(bp, 0);
	BP_SET_BYTEORDER(bp, ZFS_HOST_BYTEORDER);
}

void
ddt_key_fill(ddt_key_t *ddk, const blkptr_t *bp)
{
	ddk->ddk_cksum = bp->blk_cksum;
	ddk->ddk_prop = 0;

	ASSERT(BP_IS_ENCRYPTED(bp) || !BP_USES_CRYPT(bp));

	DDK_SET_LSIZE(ddk, BP_GET_LSIZE(bp));
	DDK_SET_PSIZE(ddk, BP_GET_PSIZE(bp));
	DDK_SET_COMPRESS(ddk, BP_GET_COMPRESS(bp));
	DDK_SET_CRYPT(ddk, BP_USES_CRYPT(bp));
}

void
ddt_phys_fill(ddt_univ_phys_t *ddp, ddt_phys_variant_t v, const blkptr_t *bp)
{
	ASSERT3U(v, !=, DDT_PHYS_NONE);

	if (v == DDT_PHYS_FLAT) {
		ASSERT(ddp->ddp_flat.ddp_phys_birth == 0);
		for (int d = 0; d < SPA_DVAS_PER_BP; d++)
			ddp->ddp_flat.ddp_dva[d] = bp->blk_dva[d];
		ddp->ddp_flat.ddp_phys_birth = BP_PHYSICAL_BIRTH(bp);
	} else {
		ASSERT(ddp->ddp_trad[v].ddp_phys_birth == 0);
		for (int d = 0; d < SPA_DVAS_PER_BP; d++)
			ddp->ddp_trad[v].ddp_dva[d] = bp->blk_dva[d];
		ddp->ddp_trad[v].ddp_phys_birth = BP_PHYSICAL_BIRTH(bp);
	}
}

void
ddt_phys_clear(ddt_univ_phys_t *ddp, ddt_phys_variant_t v)
{
	ASSERT3U(v, !=, DDT_PHYS_NONE);
	if (v == DDT_PHYS_FLAT)
		bzero(&ddp->ddp_flat, sizeof (ddp->ddp_flat));
	else
		bzero(&ddp->ddp_trad[v], sizeof (ddp->ddp_trad[v]));
}

/*
 * Copy one phys variant (one flat slot or one trad slot) from src to dst.
 * Refcnt is included in the copy.  Used to snapshot dde_phys before a write
 * so we can revert if the write fails.
 */
void
ddt_phys_copy(ddt_univ_phys_t *dst, const ddt_univ_phys_t *src,
    ddt_phys_variant_t v)
{
	ASSERT3U(v, !=, DDT_PHYS_NONE);
	if (v == DDT_PHYS_FLAT)
		dst->ddp_flat = src->ddp_flat;
	else
		dst->ddp_trad[v] = src->ddp_trad[v];
}

/*
 * Revert a phys variant's DVAs (and birth if orig was empty) back to a
 * previously-saved state.  Refcnt is NOT reverted -- the caller is
 * responsible for any refcount accounting (we don't refcount-on-error in
 * the same way upstream does, see commentary in zio_ddt_child_write_done).
 *
 * Used in write-completion error paths to undo a partial dedup-write that
 * modified dde_phys but failed before the write could commit on disk.
 */
void
ddt_phys_unextend(ddt_univ_phys_t *cur, const ddt_univ_phys_t *orig,
    ddt_phys_variant_t v)
{
	/*
	 * Use `<` (not `!=`) so smatch can establish an upper bound on v
	 * before it is used as an index into ddp_trad[] below.  Capture a
	 * pointer to the birth field in the same branch that resolves the
	 * DVA pointers, so v is never re-tested *after* being used as an
	 * array offset (which trips smatch's "testing array offset after
	 * use" check).
	 */
	ASSERT3U(v, <, DDT_PHYS_NONE);
	const dva_t *orig_dvas;
	dva_t *cur_dvas;
	uint64_t *cur_birth;
	uint64_t orig_birth;

	if (v == DDT_PHYS_FLAT) {
		cur_dvas = cur->ddp_flat.ddp_dva;
		orig_dvas = orig->ddp_flat.ddp_dva;
		cur_birth = &cur->ddp_flat.ddp_phys_birth;
		orig_birth = orig->ddp_flat.ddp_phys_birth;
	} else {
		cur_dvas = cur->ddp_trad[v].ddp_dva;
		orig_dvas = orig->ddp_trad[v].ddp_dva;
		cur_birth = &cur->ddp_trad[v].ddp_phys_birth;
		orig_birth = orig->ddp_trad[v].ddp_phys_birth;
	}
	for (int d = 0; d < SPA_DVAS_PER_BP; d++)
		cur_dvas[d] = orig_dvas[d];
	if (orig_birth == 0)
		*cur_birth = 0;
}

/*
 * Merge the DVAs from a newly-allocated "extend" write into the first
 * empty (DVA_IS_VALID() == B_FALSE) positions of an existing phys slot.
 *
 * Used when a dedup hit finds an entry with fewer DVAs than the current
 * write's zp_copies requires (DDT_FLAG_FLAT only -- a flat slot can hold
 * up to SPA_DVAS_PER_BP DVAs regardless of which zp_copies value first
 * created it, unlike trad-mode slots which are dedicated to one exact
 * copies value and are always filled all-or-nothing). Rather than
 * discarding the existing DVAs and writing a fresh full-sized block, we
 * allocate only the shortfall and splice the new DVAs in alongside the
 * ones already present.
 *
 * bp is the extend write's own (otherwise-unrelated) blkptr_t, holding
 * only the newly-allocated DVAs starting at blk_dva[0]; this function
 * does not assume how many of the slot's existing DVAs are already
 * valid, it discovers the gaps itself via DVA_IS_VALID() so it stays
 * correct even if the slot's state changed since the caller last
 * inspected it.
 *
 * Encrypted blocks are a special case: dva[2] does not hold a real DVA
 * in the usual sense (only 2 of the 3 slots are real allocations -- see
 * BP_IS_ENCRYPTED()), so it is excluded from the "first empty slot" scan
 * and copied unconditionally afterward instead.
 *
 * Birth time is only set if it was not already set: extending an
 * entry's DVA set does not change the txg it was first born in, since
 * ddt_phys_select() and existing readers identify the entry by its
 * first DVA plus that original birth time.
 */
void
ddt_phys_extend(ddt_univ_phys_t *ddp, ddt_phys_variant_t v,
    const blkptr_t *bp)
{
	/*
	 * Use `<` (not `!=`) so smatch can establish an upper bound on v
	 * before it is used as an index into ddp_trad[] below. Capture a
	 * pointer to the birth field in the same branch that resolves the
	 * DVA pointer, so v is never re-tested *after* being used as an
	 * array offset (which trips smatch's "testing array offset after
	 * use" check) -- same technique as ddt_phys_unextend() above.
	 */
	ASSERT3U(v, <, DDT_PHYS_NONE);
	dva_t *dvas;
	uint64_t *birth;

	if (v == DDT_PHYS_FLAT) {
		dvas = ddp->ddp_flat.ddp_dva;
		birth = &ddp->ddp_flat.ddp_phys_birth;
	} else {
		dvas = ddp->ddp_trad[v].ddp_dva;
		birth = &ddp->ddp_trad[v].ddp_phys_birth;
	}

	int bp_ndvas = BP_GET_NDVAS(bp);
	int ddp_max_dvas = BP_IS_ENCRYPTED(bp) ?
	    SPA_DVAS_PER_BP - 1 : SPA_DVAS_PER_BP;

	int s = 0, d = 0;
	while (s < bp_ndvas && d < ddp_max_dvas) {
		if (DVA_IS_VALID(&dvas[d])) {
			d++;
			continue;
		}
		dvas[d] = bp->blk_dva[s];
		s++; d++;
	}

	/*
	 * If the caller offered more DVAs than there was room for, the
	 * shortfall the caller computed before issuing the extend write
	 * didn't match the slot's actual state by the time this runs.
	 */
	ASSERT3U(s, ==, bp_ndvas);

	if (BP_IS_ENCRYPTED(bp))
		dvas[2] = bp->blk_dva[2];

	if (*birth == 0)
		*birth = BP_PHYSICAL_BIRTH(bp);
}

void
ddt_phys_addref(ddt_univ_phys_t *ddp, ddt_phys_variant_t v)
{
	ASSERT3U(v, !=, DDT_PHYS_NONE);
	if (v == DDT_PHYS_FLAT)
		ddp->ddp_flat.ddp_refcnt++;
	else
		ddp->ddp_trad[v].ddp_refcnt++;
}

void
ddt_phys_decref(ddt_univ_phys_t *ddp, ddt_phys_variant_t v)
{
	ASSERT3U(v, !=, DDT_PHYS_NONE);
	if (v == DDT_PHYS_FLAT) {
		ASSERT((int64_t)ddp->ddp_flat.ddp_refcnt > 0);
		ddp->ddp_flat.ddp_refcnt--;
	} else {
		ASSERT((int64_t)ddp->ddp_trad[v].ddp_refcnt > 0);
		ddp->ddp_trad[v].ddp_refcnt--;
	}
}

void
ddt_phys_free(ddt_t *ddt, ddt_key_t *ddk, ddt_univ_phys_t *ddp,
    ddt_phys_variant_t v, uint64_t txg)
{
	blkptr_t blk;

	ASSERT3U(v, !=, DDT_PHYS_NONE);
	ddt_bp_create(ddt->ddt_checksum, ddk, ddp, v, &blk);
	ddt_phys_clear(ddp, v);
	zio_free(ddt->ddt_spa, txg, &blk);
}

ddt_phys_variant_t
ddt_phys_select(const ddt_t *ddt, const ddt_entry_t *dde, const blkptr_t *bp)
{
	const ddt_univ_phys_t *ddp = dde->dde_phys;

	if (ddt->ddt_flags & DDT_FLAG_FLAT) {
		if (DVA_EQUAL(BP_IDENTITY(bp), &ddp->ddp_flat.ddp_dva[0]) &&
		    BP_PHYSICAL_BIRTH(bp) == ddp->ddp_flat.ddp_phys_birth)
			return (DDT_PHYS_FLAT);
		return (DDT_PHYS_NONE);
	}

	for (int p = 0; p < DDT_PHYS_MAX; p++) {
		if (DVA_EQUAL(BP_IDENTITY(bp), &ddp->ddp_trad[p].ddp_dva[0]) &&
		    BP_PHYSICAL_BIRTH(bp) == ddp->ddp_trad[p].ddp_phys_birth)
			return ((ddt_phys_variant_t)p);
	}
	return (DDT_PHYS_NONE);
}

uint64_t
ddt_phys_birth(const ddt_univ_phys_t *ddp, ddt_phys_variant_t v)
{
	ASSERT3U(v, <, DDT_PHYS_NONE);

	if (v == DDT_PHYS_FLAT)
		return (ddp->ddp_flat.ddp_phys_birth);
	else
		return (ddp->ddp_trad[v].ddp_phys_birth);
}

/*
 * True if the slot's first DVA is a gang block.  A DDT slot must never mix
 * gang and non-gang DVAs (an illegal "mixed" blkptr), so the write path
 * refuses to extend a ganged slot or merge a ganged DVA into a non-ganged
 * one; see zio_ddt_write().  Ported from upstream OpenZFS 5f5321e.
 */
int
ddt_phys_is_gang(const ddt_univ_phys_t *ddp, ddt_phys_variant_t v)
{
	ASSERT3U(v, <, DDT_PHYS_NONE);

	const dva_t *dvas = (v == DDT_PHYS_FLAT) ?
	    ddp->ddp_flat.ddp_dva : ddp->ddp_trad[v].ddp_dva;

	return (DVA_GET_GANG(&dvas[0]));
}

int
ddt_phys_dva_count(const ddt_univ_phys_t *ddp, ddt_phys_variant_t v,
    boolean_t encrypted)
{
	ASSERT3U(v, <, DDT_PHYS_NONE);

	const dva_t *dvas = (v == DDT_PHYS_FLAT) ?
	    ddp->ddp_flat.ddp_dva : ddp->ddp_trad[v].ddp_dva;

	return (DVA_IS_VALID(&dvas[0]) +
	    DVA_IS_VALID(&dvas[1]) +
	    DVA_IS_VALID(&dvas[2]) * !encrypted);
}

uint64_t
ddt_phys_refcnt(const ddt_univ_phys_t *ddp, ddt_phys_variant_t v)
{
	ASSERT3U(v, <, DDT_PHYS_NONE);

	if (v == DDT_PHYS_FLAT)
		return (ddp->ddp_flat.ddp_refcnt);
	else
		return (ddp->ddp_trad[v].ddp_refcnt);
}

uint64_t
ddt_phys_total_refcnt(const ddt_t *ddt, const ddt_univ_phys_t *ddp)
{
	uint64_t refcnt = 0;

	if (ddt->ddt_flags & DDT_FLAG_FLAT)
		refcnt = ddp->ddp_flat.ddp_refcnt;
	else
		for (int v = DDT_PHYS_SINGLE; v <= DDT_PHYS_TRIPLE; v++)
			refcnt += ddp->ddp_trad[v].ddp_refcnt;

	return (refcnt);
}

size_t
ddt_compress(void *src, uchar_t *dst, size_t s_len, size_t d_len)
{
	uchar_t *version = dst++;
	int cpfunc = ZIO_COMPRESS_ZLE;
	zio_compress_info_t *ci = &zio_compress_table[cpfunc];
	size_t c_len;

	ASSERT(d_len >= s_len + 1);	/* no compression plus version byte */

	c_len = ci->ci_compress(src, dst, s_len, d_len - 1, ci->ci_level);

	if (c_len == s_len) {
		cpfunc = ZIO_COMPRESS_OFF;
		bcopy(src, dst, s_len);
	}

	*version = cpfunc;
	/* CONSTCOND */
	if (ZFS_HOST_BYTEORDER)
		*version |= DDT_COMPRESS_BYTEORDER_MASK;

	return (c_len + 1);
}

void
ddt_decompress(uchar_t *src, void *dst, size_t s_len, size_t d_len)
{
	uchar_t version = *src++;
	int cpfunc = version & DDT_COMPRESS_FUNCTION_MASK;
	zio_compress_info_t *ci = &zio_compress_table[cpfunc];

	if (ci->ci_decompress != NULL)
		(void) ci->ci_decompress(src, dst, s_len, d_len, ci->ci_level);
	else
		bcopy(src, dst, d_len);

	if (((version & DDT_COMPRESS_BYTEORDER_MASK) != 0) !=
	    (ZFS_HOST_BYTEORDER != 0))
		byteswap_uint64_array(dst, d_len);
}

ddt_t *
ddt_select_by_checksum(spa_t *spa, enum zio_checksum c)
{
	return (spa->spa_ddt[c]);
}

ddt_t *
ddt_select(spa_t *spa, const blkptr_t *bp)
{
	return (spa->spa_ddt[BP_GET_CHECKSUM(bp)]);
}

void
ddt_enter(ddt_t *ddt)
{
	mutex_enter(&ddt->ddt_lock);
}

void
ddt_exit(ddt_t *ddt)
{
	mutex_exit(&ddt->ddt_lock);
}

/*
 * Lazily allocate the per-entry I/O substruct.  Idempotent.  Caller must
 * hold ddt_lock so two threads don't race to allocate it simultaneously.
 */
void
ddt_io_alloc(ddt_entry_t *dde)
{
	if (dde->dde_io != NULL)
		return;
	dde->dde_io = kmem_zalloc(sizeof (ddt_entry_io_t), KM_SLEEP);
	mutex_init(&dde->dde_io->dde_io_lock, NULL, MUTEX_DEFAULT, NULL);
}

void
ddt_io_free(ddt_entry_t *dde)
{
	if (dde->dde_io == NULL)
		return;
	for (int p = 0; p < DDT_PHYS_MAX; p++)
		ASSERT(dde->dde_io->dde_lead_zio[p] == NULL);
	if (dde->dde_io->dde_repair_abd != NULL)
		abd_free(dde->dde_io->dde_repair_abd);
	mutex_destroy(&dde->dde_io->dde_io_lock);
	kmem_free(dde->dde_io, sizeof (ddt_entry_io_t));
	dde->dde_io = NULL;
}

static ddt_entry_t *
ddt_alloc(const ddt_t *ddt, const ddt_key_t *ddk)
{
	ddt_entry_t *dde;
	size_t sz = sizeof (ddt_entry_t) + DDT_PHYS_SIZE(ddt);

	dde = kmem_zalloc(sz, KM_SLEEP);
	cv_init(&dde->dde_cv, NULL, CV_DEFAULT, NULL);

	dde->dde_key = *ddk;
	dde->dde_ddt = (ddt_t *)ddt;

	return (dde);
}

static void
ddt_free(const ddt_t *ddt, ddt_entry_t *dde)
{
	ASSERT(!dde->dde_loading);

	ddt_io_free(dde);

	cv_destroy(&dde->dde_cv);
	kmem_free(dde, sizeof (ddt_entry_t) + DDT_PHYS_SIZE(ddt));
}

void
ddt_remove(ddt_t *ddt, ddt_entry_t *dde)
{
	ASSERT(MUTEX_HELD(&ddt->ddt_lock));

	avl_remove(&ddt->ddt_tree, dde);
	ddt_free(ddt, dde);
}

static boolean_t
ddt_special_over_quota(spa_t *spa, metaslab_class_t *mc)
{
	if (mc != NULL && metaslab_class_get_space(mc) > 0) {
		/* Over quota if allocating outside of this special class */
		if (spa_syncing_txg(spa) <= spa->spa_dedup_class_full_txg +
		    dedup_class_wait_txgs) {
			/* Waiting for some deferred frees to be processed */
			return (B_TRUE);
		}

		/*
		 * We're considered over quota when we hit 85% full, or for
		 * larger drives, when there is less than 8GB free.
		 */
		uint64_t allocated = metaslab_class_get_alloc(mc);
		uint64_t capacity = metaslab_class_get_space(mc);
		uint64_t limit = MAX(capacity * 85 / 100,
		    (capacity > (1LL << 33)) ? capacity - (1LL << 33) : 0);

		return (allocated >= limit);
	}
	return (B_FALSE);
}

/*
 * Check if the DDT is over its quota.  This can be due to a few conditions:
 *   1. 'dedup_table_quota' property is not 0 (none) and the dedup dsize
 *       exceeds this limit
 *
 *   2. 'dedup_table_quota' property is set to automatic (~0ULL) and
 *      a. the dedup or special allocation class could not satisfy a DDT
 *         allocation in a recent transaction
 *      b. the dedup or special allocation class has exceeded its 85% limit
 */
static boolean_t
ddt_over_quota(spa_t *spa)
{
	if (spa->spa_dedup_table_quota == 0)
		return (B_FALSE);

	/* ~0ULL is the registered default for dedup_table_quota: automatic. */
	if (spa->spa_dedup_table_quota != ~0ULL)
		return (ddt_get_ddt_dsize(spa) > spa->spa_dedup_table_quota);

	/*
	 * For automatic quota, table size is limited by dedup or special class
	 */
	if (ddt_special_over_quota(spa, spa_dedup_class(spa)))
		return (B_TRUE);
	else if (spa_special_has_ddt(spa) &&
	    ddt_special_over_quota(spa, spa_special_class(spa)))
		return (B_TRUE);

	return (B_FALSE);
}

/*
 * If the BP passed to ddt_lookup has valid DVAs, compare them to the ones
 * in the entry.  If they differ, the passed-in BP is from a previous
 * generation of this entry (i.e. it was pruned and its key has since been
 * recreated), and we must act like the entry doesn't exist.
 *
 * This matters on the free path (zio_ddt_free) and for prune candidates
 * (prune_candidates_sync); writers pass verify=B_FALSE because an override
 * BP legitimately carries DVAs that are not in the DDT (see the comment in
 * zio_ddt_write).  Ported from upstream OpenZFS #16277 follow-up / #17120.
 */
static boolean_t
ddt_entry_lookup_is_valid(ddt_t *ddt, const blkptr_t *bp, ddt_entry_t *dde)
{
	/* If the BP has no DVAs, then this entry is good. */
	if (BP_GET_NDVAS(bp) == 0)
		return (B_TRUE);

	/*
	 * Match on block identity (DVA[0] plus physical birth), not on the
	 * BP's DVA count.
	 *
	 * A traditional entry keeps one phys per value of the copies=
	 * property the block was written with.  The BP may carry more DVAs
	 * than that: a gang header is stored in more copies than the data it
	 * gangs, so a copies=1 dedup gang block has a two-DVA header BP but
	 * lives in the copies=1 slot.  Indexing ddp_trad[] by BP_GET_NDVAS()
	 * therefore inspects the wrong slot, finds it empty, and reports a
	 * live entry as pruned -- after which zio_ddt_free() bypasses the
	 * refcount and physically frees the shared gang header on every
	 * delete.  Second and later deletes commit overlapping FREE records
	 * to the spacemaps; the gang members leak; the entry never dies.
	 */
	return (ddt_phys_select(ddt, dde, bp) != DDT_PHYS_NONE);
}

/*
 * Hide an entry that turned out not to be valid for the caller's BP:
 * eject it from the live tree unless someone else is waiting on it (the
 * last waiter ejects, mirroring the over-quota pattern).  Stored entries
 * had their histogram subtracted at load (ddt_stat_update(-1ULL)); undo
 * that on eject or the stats drift.  Log-sourced entries (dde_logged)
 * are accounted in ddt_log_histogram and need no undo.
 */
static void
ddt_lookup_eject_invalid(ddt_t *ddt, ddt_entry_t *dde)
{
	if (dde->dde_waiters != 0)
		return;
	avl_remove(&ddt->ddt_tree, dde);
	if (dde->dde_type < DDT_TYPES && !dde->dde_logged)
		ddt_stat_update(ddt, dde, 0);
	ddt_free(ddt, dde);
}

static ddt_entry_t *
ddt_lookup_impl(ddt_t *ddt, const blkptr_t *bp, ddt_class_t class_hint,
    boolean_t verify)
{
	spa_t *spa = ddt->ddt_spa;
	ddt_entry_t *dde, dde_search;
	ddt_type_t type;
	ddt_class_t class;
	avl_index_t where;
	int error;

	ASSERT(MUTEX_HELD(&ddt->ddt_lock));

	ddt_key_fill(&dde_search.dde_key, bp);

	dde = avl_find(&ddt->ddt_tree, &dde_search, &where);
	if (dde != NULL) {
		/* If we went over quota, act like we didn't find it. */
		if (dde->dde_overquota)
			return (NULL);

		/* If it's already loaded, we can just return it. */
		if (dde->dde_loaded) {
			if (!verify ||
			    ddt_entry_lookup_is_valid(ddt, bp, dde))
				return (dde);
			return (NULL);
		}

		/* Someone else is loading it, wait for it. */
		dde->dde_waiters++;
		while (dde->dde_loading)
			cv_wait(&dde->dde_cv, &ddt->ddt_lock);
		dde->dde_waiters--;

		/* Loaded but over quota; forget we were ever here. */
		if (dde->dde_overquota) {
			if (dde->dde_waiters == 0) {
				avl_remove(&ddt->ddt_tree, dde);
				ddt_free(ddt, dde);
			}
			return (NULL);
		}

		/* Not valid for this BP; hide it (last waiter ejects). */
		if (verify && !ddt_entry_lookup_is_valid(ddt, bp, dde)) {
			ddt_lookup_eject_invalid(ddt, dde);
			return (NULL);
		}

		return (dde);
	}

	/*
	 * First use of this DDT since the pool was created or imported, for a
	 * checksum that had no on-disk DDT yet: configure it now, BEFORE
	 * ddt_alloc() below reads ddt_flags to size the entry.  Otherwise the
	 * entry is allocated at the UNCONFIGURED (trad) size, the DDT later
	 * flips to FDT/flat in ddt_sync_table(), and ddt_free() then frees it
	 * at the flat size -- a kmem/umem alloc/free size-mismatch panic
	 * ("buffer freed to wrong cache").  Matches upstream OpenZFS
	 * ddt_lookup().
	 */
	if (ddt->ddt_version == DDT_VERSION_UNCONFIGURED) {
		VERIFY0(ddt_configure(ddt, B_TRUE));
		ASSERT3U(ddt->ddt_version, !=, DDT_VERSION_UNCONFIGURED);
	}

	/*
	 * Not found; create a new entry and load it.  Per upstream PR #15892
	 * c61047c, ddt_lookup always creates the in-memory entry if not found
	 * (the add=B_FALSE callers were removed during the FDT port).
	 */
	dde = ddt_alloc(ddt, &dde_search.dde_key);

	/*
	 * Record the time this class was created (used by ddt prune).  For a
	 * genuinely new entry this timestamp persists; if the entry is then
	 * loaded from the log or a store object below, the persisted
	 * ddp_class_start overwrites it.  Without this, every flat entry has
	 * class_start == 0 and "zpool ddtprune" would treat the whole unique
	 * class as infinitely old.  Ported from OpenZFS PR #16277.
	 */
	if (ddt->ddt_flags & DDT_FLAG_FLAT)
		dde->dde_phys->ddp_flat.ddp_class_start = ddt_class_start();

	avl_insert(&ddt->ddt_tree, dde, where);

	dde->dde_loading = B_TRUE;

	ddt_exit(ddt);

	/*
	 * If this DDT has a log (FDT), the entry may live only in the log and
	 * not yet be flushed to a store object.  We MUST consult the log here,
	 * before falling through to the store search below.  Otherwise a
	 * logged-but-unflushed entry is missed, ddt_lookup() returns a fresh
	 * empty dde, and the caller creates a duplicate entry or loses a free
	 * -- producing leaked space, wrong DVAs (ECKSUM), and cross-log class
	 * drift (ddt_object_remove ENOENT in ddt_sync_flush_entry()).  Ported
	 * from upstream OpenZFS PR #16277 P1 (ddt_log_find_key()).
	 */
	if (ddt->ddt_flags & DDT_FLAG_LOG) {
		ddt_lightweight_entry_t ddlwe;
		boolean_t from_flushing;
		if (ddt_log_find_key(ddt, &dde_search.dde_key, &ddlwe,
		    &from_flushing)) {
			ddt_enter(ddt);
			dde->dde_type = ddlwe.ddlwe_type;
			dde->dde_class = ddlwe.ddlwe_class;
			memcpy(dde->dde_phys, &ddlwe.ddlwe_phys,
			    DDT_PHYS_SIZE(ddt));
			dde->dde_loaded = B_TRUE;
			dde->dde_logged = B_TRUE;
			dde->dde_from_flushing = from_flushing;
			dde->dde_loading = B_FALSE;
			cv_broadcast(&dde->dde_cv);

			/* Log entry not valid for this BP; hide it. */
			if (verify &&
			    !ddt_entry_lookup_is_valid(ddt, bp, dde)) {
				ddt_lookup_eject_invalid(ddt, dde);
				return (NULL);
			}

			return (dde);
		}
	}

	error = ENOENT;

	for (type = 0; type < DDT_TYPES; type++) {
		/*
		 * Try the caller's hinted class first. Prune is currently
		 * the only hinted caller, and it only ever queues
		 * DDT_CLASS_UNIQUE candidates -- without the hint, the class
		 * loop below tries DITTO and DUPLICATE first and always
		 * misses both (2 wasted ZAP lookups) before reaching UNIQUE,
		 * for every single prune candidate. class_hint == DDT_CLASSES
		 * means "no hint": every existing caller keeps the original
		 * unordered scan below untouched.
		 */
		if (class_hint < DDT_CLASSES) {
			error = ddt_object_lookup(ddt, type, class_hint, dde);
			if (error != ENOENT) {
				class = class_hint;
				ASSERT0(error);
				break;
			}
		}
		for (class = 0; class < DDT_CLASSES; class++) {
			if (class == class_hint)
				continue;	/* already tried above */
			error = ddt_object_lookup(ddt, type, class, dde);
			if (error != ENOENT) {
				ASSERT0(error);
				break;
			}
		}
		if (error != ENOENT)
			break;
	}

	ddt_enter(ddt);

	ASSERT(!dde->dde_loaded);
	ASSERT(dde->dde_loading);

	dde->dde_type = type;	/* will be DDT_TYPES if no entry found */
	dde->dde_class = class;	/* will be DDT_CLASSES if no entry found */

	if (dde->dde_type == DDT_TYPES &&
	    dde->dde_class == DDT_CLASSES &&
	    ddt_over_quota(spa)) {
		/* Over quota.  If no one is waiting, clean up right now. */
		if (dde->dde_waiters == 0) {
			avl_remove(&ddt->ddt_tree, dde);
			dde->dde_loading = B_FALSE;
			ddt_free(ddt, dde);
			return (NULL);
		}

		/* Flag cleanup required; last waiter frees it. */
		dde->dde_overquota = B_TRUE;
	} else if (error == 0) {
		ddt_stat_update(ddt, dde, -1ULL);
	}

	dde->dde_loaded = B_TRUE;
	dde->dde_loading = B_FALSE;

	cv_broadcast(&dde->dde_cv);

	if (dde->dde_overquota)
		return (NULL);

	/*
	 * Whatever we found -- or the fresh empty entry we just created --
	 * may not be valid for this BP; if so, hide it (and eject it if
	 * nobody else is waiting on it).  For zio_ddt_free() this is the
	 * pruned-entry case: returning NULL routes the free to its direct
	 * (non-DDT) fallback.
	 */
	if (verify && !ddt_entry_lookup_is_valid(ddt, bp, dde)) {
		ddt_lookup_eject_invalid(ddt, dde);
		return (NULL);
	}

	return (dde);
}

ddt_entry_t *
ddt_lookup(ddt_t *ddt, const blkptr_t *bp, boolean_t verify)
{
	return (ddt_lookup_impl(ddt, bp, DDT_CLASSES, verify));
}

/*
 * Same as ddt_lookup(), but hints that the entry is expected to be in
 * DDT_CLASS_UNIQUE -- true by construction for every candidate queued by
 * ddt_prune_walk(), which only ever walks the unique class. Used by
 * prune_candidates_sync() to avoid the wasted DITTO/DUPLICATE probes that
 * ddt_lookup()'s unordered class scan would otherwise do before reaching
 * UNIQUE on every single prune candidate.
 */
static ddt_entry_t *
ddt_lookup_unique(ddt_t *ddt, const blkptr_t *bp)
{
	return (ddt_lookup_impl(ddt, bp, DDT_CLASS_UNIQUE, B_TRUE));
}

void
ddt_prefetch(spa_t *spa, const blkptr_t *bp)
{
	ddt_t *ddt;
	uint64_t ddebuf[DDT_ENTRY_BUF_WORDS] = { 0 };
	ddt_entry_t *dde = (ddt_entry_t *)ddebuf;

	if (!zfs_dedup_prefetch || bp == NULL || !BP_GET_DEDUP(bp))
		return;

	/*
	 * We only remove the DDT once all tables are empty and only
	 * prefetch dedup blocks when there are entries in the DDT.
	 * Thus no locking is required as the DDT can't disappear on us.
	 */
	ddt = ddt_select(spa, bp);
	ddt_key_fill(&dde->dde_key, bp);

	for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
		for (ddt_class_t class = 0; class < DDT_CLASSES; class++) {
			ddt_object_prefetch(ddt, type, class, dde);
		}
	}
}

void
ddt_prefetch_all(spa_t *spa)
{
	/*
	 * Load all DDT entries for each type/class combination. This is
	 * intended to perform a prefetch on all such blocks. For the same
	 * reason that ddt_prefetch isn't locked, this is also not locked.
	 */
	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		ddt_t *ddt = spa->spa_ddt[c];
		if (!ddt)
			continue;

		for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
			for (ddt_class_t class = 0; class < DDT_CLASSES;
			    class++) {
				ddt_object_prefetch_all(ddt, type, class);
			}
		}
	}
}

/*
 * Opaque struct used for ddt_key comparison
 */
#define	DDT_KEY_CMP_LEN	(sizeof (ddt_key_t) / sizeof (uint16_t))

typedef struct ddt_key_cmp {
	uint16_t	u16[DDT_KEY_CMP_LEN];
} ddt_key_cmp_t;

int
ddt_key_compare(const void *x1, const void *x2)
{
	const ddt_key_cmp_t *k1 = (const ddt_key_cmp_t *)x1;
	const ddt_key_cmp_t *k2 = (const ddt_key_cmp_t *)x2;
	int32_t cmp = 0;

	for (int i = 0; i < DDT_KEY_CMP_LEN; i++) {
		cmp = (int32_t)k1->u16[i] - (int32_t)k2->u16[i];
		if (likely(cmp))
			break;
	}

	return (TREE_ISIGN(cmp));
}

int
ddt_entry_compare(const void *x1, const void *x2)
{
	const ddt_entry_t *dde1 = x1;
	const ddt_entry_t *dde2 = x2;

	return (ddt_key_compare(&dde1->dde_key, &dde2->dde_key));
}

void
ddt_init(void)
{
	/*
	 * illumos has no ddt_entry kmem cache (DDEs are kmem_zalloc'd with a
	 * flexible dde_phys array), so module init only sets up the dedup log
	 * caches (ddt_log.c).
	 */
	ddt_log_init();
}

void
ddt_fini(void)
{
	ddt_log_fini();
}

static ddt_t *
ddt_table_alloc(spa_t *spa, enum zio_checksum c)
{
	ddt_t *ddt;

	ddt = kmem_zalloc(sizeof (*ddt), KM_SLEEP);

	mutex_init(&ddt->ddt_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&ddt->ddt_tree, ddt_entry_compare,
	    sizeof (ddt_entry_t), offsetof(ddt_entry_t, dde_node));
	avl_create(&ddt->ddt_repair_tree, ddt_entry_compare,
	    sizeof (ddt_entry_t), offsetof(ddt_entry_t, dde_node));
	rw_init(&ddt->ddt_objects_lock, NULL, RW_DEFAULT, NULL);
	ddt->ddt_checksum = c;
	ddt->ddt_version = DDT_VERSION_UNCONFIGURED;
	ddt->ddt_flags = 0;
	ddt->ddt_dir_object = 0;
	ddt->ddt_spa = spa;
	ddt->ddt_os = spa->spa_meta_objset;
	ddt->ddt_log_flush_pressure = 10;

	ddt_log_alloc(ddt);

	return (ddt);
}

static void
ddt_table_free(ddt_t *ddt)
{
	ASSERT(avl_numnodes(&ddt->ddt_tree) == 0);
	ASSERT(avl_numnodes(&ddt->ddt_repair_tree) == 0);
	ddt_log_free(ddt);
	for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
		for (ddt_class_t class = 0; class < DDT_CLASSES; class++) {
			if (ddt->ddt_object_dnode[type][class] != NULL) {
				dnode_rele(ddt->ddt_object_dnode[type][class],
				    ddt);
				ddt->ddt_object_dnode[type][class] = NULL;
			}
		}
	}
	avl_destroy(&ddt->ddt_tree);
	avl_destroy(&ddt->ddt_repair_tree);
	rw_destroy(&ddt->ddt_objects_lock);
	mutex_destroy(&ddt->ddt_lock);
	kmem_free(ddt, sizeof (*ddt));
}

void
ddt_create(spa_t *spa)
{
	spa->spa_dedup_checksum = ZIO_DEDUPCHECKSUM;
	spa->spa_dedup_dsize = ~0ULL;

	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		ddt_t *ddt = ddt_table_alloc(spa, c);
		spa->spa_ddt[c] = ddt;
	}
	/*
	 * Layout selection is LAZY: each table stays
	 * DDT_VERSION_UNCONFIGURED until its first ddt_lookup(), which
	 * calls ddt_configure() using the feature state at that moment
	 * (matching upstream OpenZFS).  Eagerly configuring every table
	 * here locked all 14 checksums to the pool-creation feature
	 * state, so a table first used after
	 * `zpool set feature@fast_dedup=enabled` was stuck LEGACY and
	 * the feature could never become active.
	 */
}

int
ddt_load(spa_t *spa)
{
	int error;

	ddt_create(spa);

	error = zap_lookup(spa->spa_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_DDT_STATS, sizeof (uint64_t), 1,
	    &spa->spa_ddt_stat_object);

	if (error)
		return (error == ENOENT ? 0 : error);

	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		ddt_t *ddt = spa->spa_ddt[c];
		/* Figure out version/flags/dir_object from what's on disk. */
		error = ddt_configure(ddt, B_FALSE);
		if (error == ENOENT) {
			/* Never written before; defer to first use. */
			error = 0;
			continue;
		}
		if (error != 0)
			return (error);
		for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
			for (ddt_class_t class = 0; class < DDT_CLASSES;
			    class++) {
				error = ddt_object_load(ddt, type, class);
				if (error != 0 && error != ENOENT)
					return (error);
			}
		}

		/* Replay the on-disk dedup log (FDT/fast_dedup only). */
		if (ddt->ddt_flags & DDT_FLAG_LOG) {
			error = ddt_log_load(ddt);
			if (error != 0 && error != ENOENT)
				return (error);
		}

		/*
		 * Seed the cached histograms.
		 */
		bcopy(ddt->ddt_histogram, &ddt->ddt_histogram_cache,
		    sizeof (ddt->ddt_histogram));
	}

	return (0);
}

void
ddt_unload(spa_t *spa)
{
	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		if (spa->spa_ddt[c]) {
			ddt_table_free(spa->spa_ddt[c]);
			spa->spa_ddt[c] = NULL;
		}
	}
}

boolean_t
ddt_class_contains(spa_t *spa, ddt_class_t max_class, const blkptr_t *bp)
{
	ddt_t *ddt;
	uint64_t ddebuf[DDT_ENTRY_BUF_WORDS] = { 0 };
	ddt_entry_t *dde = (ddt_entry_t *)ddebuf;

	if (!BP_GET_DEDUP(bp))
		return (B_FALSE);

	if (max_class == DDT_CLASS_UNIQUE)
		return (B_TRUE);

	ddt = spa->spa_ddt[BP_GET_CHECKSUM(bp)];

	ddt_key_fill(&dde->dde_key, bp);

	for (ddt_type_t type = 0; type < DDT_TYPES; type++)
		for (ddt_class_t class = 0; class <= max_class; class++)
			if (ddt_object_lookup(ddt, type, class, dde) == 0)
				return (B_TRUE);

	return (B_FALSE);
}

ddt_entry_t *
ddt_repair_start(ddt_t *ddt, const blkptr_t *bp)
{
	ddt_key_t ddk;
	ddt_entry_t *dde;

	ddt_key_fill(&ddk, bp);

	dde = ddt_alloc(ddt, &ddk);

	for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
		for (ddt_class_t class = 0; class < DDT_CLASSES; class++) {
			/*
			 * We can only do repair if there are multiple copies
			 * of the block.  For anything in the UNIQUE class,
			 * there's definitely only one copy, so don't even try.
			 */
			if (class != DDT_CLASS_UNIQUE &&
			    ddt_object_lookup_open(ddt, type, class, dde) == 0)
				return (dde);
		}
	}

	bzero(dde->dde_phys, DDT_PHYS_SIZE(ddt));

	return (dde);
}

void
ddt_repair_done(ddt_t *ddt, ddt_entry_t *dde)
{
	avl_index_t where;

	ddt_enter(ddt);

	if (DDE_REPAIR_ABD(dde) != NULL && spa_writeable(ddt->ddt_spa) &&
	    avl_find(&ddt->ddt_repair_tree, dde, &where) == NULL)
		avl_insert(&ddt->ddt_repair_tree, dde, where);
	else
		ddt_free(ddt, dde);

	ddt_exit(ddt);
}

static void
ddt_repair_entry_done(zio_t *zio)
{
	ddt_entry_t *rdde = zio->io_private;

	/* derive ddt via the back-pointer set in ddt_alloc */
	ddt_free(rdde->dde_ddt, rdde);
}

static void
ddt_repair_entry(ddt_t *ddt, ddt_entry_t *dde, ddt_entry_t *rdde, zio_t *rio)
{
	ddt_univ_phys_t *ddp = dde->dde_phys;
	ddt_univ_phys_t *rddp = rdde->dde_phys;
	ddt_key_t *ddk = &dde->dde_key;
	ddt_key_t *rddk = &rdde->dde_key;
	zio_t *zio;
	blkptr_t blk;

	zio = zio_null(rio, rio->io_spa, NULL,
	    ddt_repair_entry_done, rdde, rio->io_flags);

	int nphys = DDT_NPHYS(ddt);
	for (int p = 0; p < nphys; p++) {
		ddt_phys_variant_t v = DDT_PHYS_VARIANT(ddt, p);
		ddt_phys_t *dp = DDT_PHYS_SLOT(ddp, v);
		ddt_phys_t *rdp = DDT_PHYS_SLOT(rddp, v);
		if (dp->ddp_phys_birth == 0 ||
		    dp->ddp_phys_birth != rdp->ddp_phys_birth ||
		    bcmp(dp->ddp_dva, rdp->ddp_dva, sizeof (dp->ddp_dva)))
			continue;
		ddt_bp_create(ddt->ddt_checksum, ddk, ddp, v, &blk);
		zio_nowait(zio_rewrite(zio, zio->io_spa, 0, &blk,
		    rdde->dde_io->dde_repair_abd, DDK_GET_PSIZE(rddk),
		    NULL, NULL,
		    ZIO_PRIORITY_SYNC_WRITE, ZIO_DDT_CHILD_FLAGS(zio), NULL));
	}

	zio_nowait(zio);
}

static void
ddt_repair_table(ddt_t *ddt, zio_t *rio)
{
	spa_t *spa = ddt->ddt_spa;
	ddt_entry_t *dde, *rdde_next, *rdde;
	avl_tree_t *t = &ddt->ddt_repair_tree;
	blkptr_t blk;

	if (spa_sync_pass(spa) > 1)
		return;

	ddt_enter(ddt);
	for (rdde = avl_first(t); rdde != NULL; rdde = rdde_next) {
		rdde_next = AVL_NEXT(t, rdde);
		avl_remove(&ddt->ddt_repair_tree, rdde);
		ddt_exit(ddt);
		ddt_bp_create(ddt->ddt_checksum, &rdde->dde_key, NULL,
		    DDT_PHYS_NONE, &blk);
		dde = ddt_repair_start(ddt, &blk);
		ddt_repair_entry(ddt, dde, rdde, rio);
		ddt_repair_done(ddt, dde);
		ddt_enter(ddt);
	}
	ddt_exit(ddt);
}

static void
ddt_sync_entry(ddt_t *ddt, ddt_entry_t *dde, dmu_tx_t *tx, uint64_t txg)
{
	dsl_pool_t *dp = ddt->ddt_spa->spa_dsl_pool;
	ddt_univ_phys_t *ddp = dde->dde_phys;
	ddt_key_t *ddk = &dde->dde_key;
	ddt_type_t otype = dde->dde_type;
	ddt_type_t ntype = DDT_TYPE_CURRENT;
	ddt_class_t oclass = dde->dde_class;
	ddt_class_t nclass;
	uint64_t total_refcnt = 0;

	ASSERT(dde->dde_loaded);
	ASSERT(!dde->dde_loading);

	if (ddt->ddt_flags & DDT_FLAG_FLAT) {
		ddt_phys_t *slot = (ddt_phys_t *)&ddp->ddp_flat;
		ASSERT(DDE_LEAD_ZIO(dde, 0) == NULL);
		ASSERT((int64_t)slot->ddp_refcnt >= 0);
		if (slot->ddp_phys_birth != 0) {
			if (slot->ddp_refcnt == 0)
				ddt_phys_free(ddt, ddk, ddp,
				    DDT_PHYS_FLAT, txg);
			total_refcnt = slot->ddp_refcnt;
		}
	} else {
		for (int p = 0; p < DDT_PHYS_MAX; p++) {
			ddt_phys_t *slot = &ddp->ddp_trad[p];
			ASSERT(DDE_LEAD_ZIO(dde, p) == NULL);
			ASSERT((int64_t)slot->ddp_refcnt >= 0);
			if (slot->ddp_phys_birth == 0) {
				ASSERT(slot->ddp_refcnt == 0);
				continue;
			}
			if (p == DDT_PHYS_DITTO) {
				/*
				 * New ditto copies are no longer written
				 * (the dedupditto property is a no-op), but
				 * old pools may still hold them.  Don't keep
				 * the obsolete slot regardless of refcount,
				 * and don't leak it either: free it.
				 * Matches upstream OpenZFS.
				 */
				ddt_phys_free(ddt, ddk, ddp, p, txg);
				continue;
			}
			if (slot->ddp_refcnt == 0)
				ddt_phys_free(ddt, ddk, ddp, p, txg);
			total_refcnt += slot->ddp_refcnt;
		}
	}

	/*
	 * The ditto slot was freed above, so no entry is (re)classified as
	 * DDT_CLASS_DITTO any more; entries an old pool keeps in that class
	 * move to unique/duplicate the next time they are synced.
	 */
	if (total_refcnt > 1)
		nclass = DDT_CLASS_DUPLICATE;
	else
		nclass = DDT_CLASS_UNIQUE;

	if (otype != DDT_TYPES &&
	    (otype != ntype || oclass != nclass || total_refcnt == 0)) {
		ddt_class_t rclass = oclass;

		if (unlikely(ddt_inject_stray_class))
			rclass = ddt_stray_class(oclass);

		/*
		 * Anything other than ENOENT (an I/O error, say) is
		 * still fatal here; only class/type drift is recovered.
		 */
		VERIFY0(ddt_object_remove_sync(ddt, otype, rclass,
		    &dde->dde_key, tx));
		ASSERT(ddt_object_lookup(ddt, otype, oclass, dde) == ENOENT);
	}

	if (total_refcnt != 0) {
		dde->dde_type = ntype;
		dde->dde_class = nclass;
		ddt_stat_update(ddt, dde, 0);
		if (!ddt_object_exists(ddt, ntype, nclass))
			ddt_object_create(ddt, ntype, nclass, tx);
		ddt_lightweight_entry_t ddlwe;
		ddlwe.ddlwe_key = dde->dde_key;
		ddlwe.ddlwe_type = ntype;
		ddlwe.ddlwe_class = nclass;
		memcpy(&ddlwe.ddlwe_phys, dde->dde_phys, DDT_PHYS_SIZE(ddt));
		VERIFY(ddt_object_update(ddt, ntype, nclass, &ddlwe, tx) == 0);

		/*
		 * If the class changes, the order that we scan this bp
		 * changes.  If it decreases, we could miss it, so
		 * scan it right now.  (This covers both class changing
		 * while we are doing ddt_walk(), and when we are
		 * traversing.)
		 */
		if (nclass < oclass) {
			dsl_scan_ddt_entry(dp->dp_scan,
			    ddt->ddt_checksum, ddt, &ddlwe, tx);
		}
	}
}

/*
 * Tunables for the dedup log flusher (ddt_sync_flush_log()).  Plain globals
 * in illumos; upstream exposes them as module params.  Backlog-paced
 * single-pass flusher ported from OpenZFS 1b495ee (PR #17038).
 */
uint_t zfs_dedup_log_flush_min_time_ms = 1000;
uint_t zfs_dedup_log_flush_entries_min = 200;
uint_t zfs_dedup_log_flush_entries_max = UINT32_MAX;
uint_t zfs_dedup_log_flush_txgs = 100;
uint_t zfs_dedup_log_cap = UINT32_MAX;
boolean_t zfs_dedup_log_hard_cap = B_FALSE;
uint_t zfs_dedup_log_flush_flow_rate_txgs = 10;

extern int zfs_txg_timeout;

static void
ddt_sync_update_stats(ddt_t *ddt, dmu_tx_t *tx)
{
	/*
	 * Count all the entries stored for each type/class, and update the
	 * stats within (ddt_object_sync()).  If there are no entries for the
	 * type/class, the whole object is removed.  If all objects for the DDT
	 * are removed, its containing dir is removed, resetting the DDT to an
	 * empty slate.
	 */
	uint64_t count = 0;
	for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
		uint64_t tcount = 0;
		for (ddt_class_t class = 0; class < DDT_CLASSES; class++) {
			if (ddt_object_exists(ddt, type, class)) {
				uint64_t add;
				ddt_object_sync(ddt, type, class, tx);
				VERIFY0(ddt_object_count(ddt, type,
				    class, &add));
				tcount += add;
			}
		}
		for (ddt_class_t class = 0; class < DDT_CLASSES; class++) {
			if (tcount == 0 && ddt_object_exists(ddt, type, class))
				ddt_object_destroy(ddt, type, class, tx);
		}
		count += tcount;
	}

	if (ddt->ddt_flags & DDT_FLAG_LOG) {
		/* Include logged entries in the total count. */
		count += avl_numnodes(&ddt->ddt_log_active->ddl_tree);
		count += avl_numnodes(&ddt->ddt_log_flushing->ddl_tree);
	}

	if (count == 0) {
		/*
		 * No entries left on the DDT, so reset the version for next
		 * time.  This allows us to handle the feature being changed
		 * since the DDT was originally created.  New entries should
		 * get whatever the feature currently demands.
		 */
		if (ddt->ddt_version == DDT_VERSION_FDT)
			ddt_destroy_dir(ddt, tx);

		ddt->ddt_version = DDT_VERSION_UNCONFIGURED;
		ddt->ddt_flags = 0;
	}

	bcopy(ddt->ddt_histogram, &ddt->ddt_histogram_cache,
	    sizeof (ddt->ddt_histogram));
}

static void
ddt_sync_scan_entry(ddt_t *ddt, ddt_lightweight_entry_t *ddlwe, dmu_tx_t *tx)
{
	dsl_pool_t *dp = ddt->ddt_spa->spa_dsl_pool;

	/*
	 * Compute the target class, so we can decide whether or not to inform
	 * the scrub traversal.  Note that we don't store this in the entry, as
	 * it might change multiple times before finally being committed (if
	 * we're logging).  Instead, we recompute it in ddt_sync_entry().
	 */
	uint64_t refcnt = ddt_phys_total_refcnt(ddt, &ddlwe->ddlwe_phys);
	ddt_class_t nclass =
	    (refcnt > 1) ? DDT_CLASS_DUPLICATE : DDT_CLASS_UNIQUE;

	/*
	 * If the class changes, the order that we scan this bp changes.  If it
	 * decreases, we could miss it, so scan it right now.  (This covers both
	 * class changing while we are doing ddt_walk(), and when we are
	 * traversing.)
	 *
	 * We also do this when the refcnt goes to zero, because that change is
	 * only in the log so far; the blocks on disk won't be freed until the
	 * log is flushed, and the refcnt might increase before that.  If it
	 * does, then we could miss it in the same way.
	 */
	if (refcnt == 0 || nclass < ddlwe->ddlwe_class)
		dsl_scan_ddt_entry(dp->dp_scan, ddt->ddt_checksum, ddt,
		    ddlwe, tx);
}

static void
ddt_sync_flush_entry(ddt_t *ddt, ddt_lightweight_entry_t *ddlwe,
    ddt_type_t otype, ddt_class_t oclass, dmu_tx_t *tx)
{
	ddt_key_t *ddk = &ddlwe->ddlwe_key;
	ddt_type_t ntype = DDT_TYPE_DEFAULT;
	uint64_t refcnt = 0;

	/*
	 * Compute the total refcnt.  Along the way, issue frees for any DVAs
	 * we no longer want.
	 */
	for (int p = 0; p < DDT_NPHYS(ddt); p++) {
		ddt_univ_phys_t *ddp = &ddlwe->ddlwe_phys;
		ddt_phys_variant_t v = DDT_PHYS_VARIANT(ddt, p);
		uint64_t phys_refcnt = ddt_phys_refcnt(ddp, v);

		if (ddt_phys_birth(ddp, v) == 0) {
			ASSERT3U(phys_refcnt, ==, 0);
			continue;
		}
		if (DDT_PHYS_IS_DITTO(ddt, p)) {
			/*
			 * We don't want to keep any obsolete slots (eg ditto),
			 * regardless of their refcount, but we don't want to
			 * leak them either.  So, free them.
			 */
			ddt_phys_free(ddt, ddk, ddp, v, tx->tx_txg);
			continue;
		}
		if (phys_refcnt == 0)
			/* No remaining references, free it! */
			ddt_phys_free(ddt, ddk, ddp, v, tx->tx_txg);
		refcnt += phys_refcnt;
	}

	/* Select the best class for the entry. */
	ddt_class_t nclass =
	    (refcnt > 1) ? DDT_CLASS_DUPLICATE : DDT_CLASS_UNIQUE;

	/*
	 * If an existing entry changed type or class, or its refcount reached
	 * zero, delete it from the DDT object.
	 */
	if (otype != DDT_TYPES &&
	    (otype != ntype || oclass != nclass || refcnt == 0)) {
		ddt_class_t rclass = unlikely(ddt_inject_stray_class) ?
		    ddt_stray_class(oclass) : oclass;

		VERIFY0(ddt_object_remove_sync(ddt, otype, rclass, ddk, tx));
		ASSERT(ddt_object_contains(ddt, otype, oclass, ddk) == ENOENT);
	}

	/* Add or update the entry. */
	if (refcnt != 0) {
		ddt_histogram_t *ddh = &ddt->ddt_histogram[ntype][nclass];

		ddt_histogram_add_entry(ddt, ddh, ddlwe);

		if (!ddt_object_exists(ddt, ntype, nclass))
			ddt_object_create(ddt, ntype, nclass, tx);
		VERIFY0(ddt_object_update(ddt, ntype, nclass, ddlwe, tx));
	}
}

static int32_t
_ewma(int32_t val, int32_t prev, uint32_t weight)
{
	ASSERT3U(val, >=, 0);
	ASSERT3U(prev, >=, 0);
	const int32_t new =
	    MAX(0, prev + (val-prev) / (int32_t)MAX(weight, 1));
	ASSERT3U(new, >=, 0);
	return (new);
}

static inline void
ddt_flush_force_update_txg(ddt_t *ddt, uint64_t txg)
{
	/*
	 * If we're not forcing flush, and not being asked to start, then
	 * there's nothing more to do.
	 */
	if (txg == 0) {
		/* Update requested, are we currently forcing flush? */
		if (ddt->ddt_flush_force_txg == 0)
			return;
		txg = ddt->ddt_flush_force_txg;
	}

	/*
	 * If either of the logs have unflushed entries before the wanted txg,
	 * set the force txg, otherwise clear it.
	 */
	if ((!avl_is_empty(&ddt->ddt_log_active->ddl_tree) &&
	    ddt->ddt_log_active->ddl_first_txg <= txg) ||
	    (!avl_is_empty(&ddt->ddt_log_flushing->ddl_tree) &&
	    ddt->ddt_log_flushing->ddl_first_txg <= txg)) {
		ddt->ddt_flush_force_txg = txg;
		return;
	}

	/*
	 * Nothing to flush behind the given txg, so we can clear force flush
	 * state.
	 */
	ddt->ddt_flush_force_txg = 0;
}

static void
ddt_sync_flush_log(ddt_t *ddt, dmu_tx_t *tx)
{
	spa_t *spa = ddt->ddt_spa;
	ASSERT(avl_is_empty(&ddt->ddt_tree));

	/*
	 * Don't do any flushing when the pool is ready to shut down, or in
	 * passes beyond the first.
	 */
	if (spa_sync_pass(spa) > 1 || tx->tx_txg > spa_final_dirty_txg(spa))
		return;

	hrtime_t flush_start = gethrtime();
	uint32_t count = 0;

	/*
	 * How many entries we need to flush.  We need to at least match the
	 * ingest rate, and also consider the current backlog of entries.
	 */
	uint64_t backlog = avl_numnodes(&ddt->ddt_log_flushing->ddl_tree) +
	    avl_numnodes(&ddt->ddt_log_active->ddl_tree);

	if (avl_is_empty(&ddt->ddt_log_flushing->ddl_tree))
		goto housekeeping;

	uint64_t txgs = MAX(1, zfs_dedup_log_flush_txgs);
	uint64_t cap = MAX(1, zfs_dedup_log_cap);
	uint64_t flush_min = MAX(backlog / txgs,
	    zfs_dedup_log_flush_entries_min);

	/*
	 * The theory for this block is that if we increase the pressure while
	 * we're growing above the cap, and remove it when we're significantly
	 * below the cap, we'll stay near cap while not bouncing around too
	 * much.
	 *
	 * The factor of 10 is to smooth the pressure effect by expressing it
	 * in tenths.  The addition of the cap to the backlog in the second
	 * block is to round up, instead of down.  We never let the pressure go
	 * below 1 (10 tenths).
	 */
	if (cap != UINT32_MAX && backlog > cap &&
	    backlog > ddt->ddt_log_flush_prev_backlog) {
		ddt->ddt_log_flush_pressure += 10 * backlog / cap;
	} else if (cap != UINT32_MAX && backlog < cap) {
		ddt->ddt_log_flush_pressure -=
		    11 - (((10 * backlog) + cap - 1) / cap);
		ddt->ddt_log_flush_pressure =
		    MAX(ddt->ddt_log_flush_pressure, 10);
	}

	if (zfs_dedup_log_hard_cap && cap != UINT32_MAX)
		flush_min = MAX(flush_min, MIN(backlog - cap,
		    (flush_min * ddt->ddt_log_flush_pressure) / 10));

	uint64_t flush_max;

	/*
	 * If we've been asked to flush everything in a hurry, try to dump as
	 * much as possible on this txg.  In this case we're only limited by
	 * time, not amount.
	 *
	 * Otherwise, if we are over the cap, try to get back down to it.
	 *
	 * Finally if there is no cap (or no pressure), just set the max a
	 * little higher than the min to help smooth out variations in flush
	 * times.
	 */
	if (ddt->ddt_flush_force_txg > 0)
		flush_max = avl_numnodes(&ddt->ddt_log_flushing->ddl_tree);
	else if (cap != UINT32_MAX && !zfs_dedup_log_hard_cap)
		flush_max = MAX(flush_min * 5 / 4, MIN(backlog - cap,
		    (flush_min * ddt->ddt_log_flush_pressure) / 10));
	else
		flush_max = flush_min * 5 / 4;
	flush_max = MIN(flush_max, zfs_dedup_log_flush_entries_max);

	/*
	 * When the pool is busy or someone is explicitly waiting for this txg
	 * to complete, use the zfs_dedup_log_flush_min_time_ms.  Otherwise use
	 * half of the time in the txg timeout.
	 */
	uint64_t target_time;

	if (txg_sync_waiting(ddt->ddt_spa->spa_dsl_pool) ||
	    vdev_queue_pool_busy(spa)) {
		target_time = MIN(MSEC2NSEC(zfs_dedup_log_flush_min_time_ms),
		    SEC2NSEC(zfs_txg_timeout) / 2);
	} else {
		target_time = SEC2NSEC(zfs_txg_timeout) / 2;
	}

	ddt_lightweight_entry_t ddlwe;
	while (ddt_log_take_first(ddt, ddt->ddt_log_flushing, &ddlwe)) {
		ddt_sync_flush_entry(ddt, &ddlwe,
		    ddlwe.ddlwe_type, ddlwe.ddlwe_class, tx);

		/* End if we've synced as much as we needed to. */
		if (++count >= flush_max)
			break;

		/*
		 * As long as we've flushed the absolute minimum, stop if we're
		 * way over our target time.
		 */
		uint64_t diff = gethrtime() - flush_start;
		if (count > zfs_dedup_log_flush_entries_min &&
		    diff >= target_time * 2)
			break;

		/*
		 * End if we've passed the minimum flush and we're out of time.
		 */
		if (count > flush_min && diff >= target_time)
			break;
	}

	if (avl_is_empty(&ddt->ddt_log_flushing->ddl_tree)) {
		/* We emptied it, so truncate on-disk */
		ddt_log_truncate(ddt, tx);
	} else {
		/* More to do next time, save checkpoint */
		ddt_log_checkpoint(ddt, &ddlwe, tx);
	}

	ddt_sync_update_stats(ddt, tx);

housekeeping:
	if (avl_is_empty(&ddt->ddt_log_flushing->ddl_tree) &&
	    !avl_is_empty(&ddt->ddt_log_active->ddl_tree)) {
		/*
		 * No more to flush, and the active list has stuff, so try to
		 * swap the logs for next time.
		 */
		(void) ddt_log_swap(ddt, tx);
	}

	/* If force flush is no longer necessary, turn it off. */
	ddt_flush_force_update_txg(ddt, 0);

	ddt->ddt_log_flush_prev_backlog = backlog;

	/*
	 * Update flush rate.  This is an exponential weighted moving average
	 * of the number of entries flushed over recent txgs.
	 */
	ddt->ddt_log_flush_rate = _ewma(count, ddt->ddt_log_flush_rate,
	    zfs_dedup_log_flush_flow_rate_txgs);

	/*
	 * Update flush time rate.  This is an exponential weighted moving
	 * average of the total time taken to flush over recent txgs.
	 */
	ddt->ddt_log_flush_time_rate = _ewma(ddt->ddt_log_flush_time_rate,
	    (int32_t)NSEC2MSEC(gethrtime() - flush_start),
	    zfs_dedup_log_flush_flow_rate_txgs);
}

static void
ddt_sync_table_log(ddt_t *ddt, dmu_tx_t *tx)
{
	uint64_t count = avl_numnodes(&ddt->ddt_tree);

	if (count > 0) {
		ddt_log_update_t dlu = {0};
		ddt_log_begin(ddt, count, tx, &dlu);

		ddt_entry_t *dde;
		void *cookie = NULL;
		ddt_lightweight_entry_t ddlwe;
		while ((dde =
		    avl_destroy_nodes(&ddt->ddt_tree, &cookie)) != NULL) {
			ASSERT(dde->dde_loaded);
			DDT_ENTRY_TO_LIGHTWEIGHT(ddt, dde, &ddlwe);
			/*
			 * If this entry was rehydrated from the flushing log,
			 * drop the now-stale flushing copy before re-logging it
			 * to the active log.  Otherwise the flush pass later
			 * processes the stale copy with its old type/class and
			 * over-frees or hits ddt_object_remove() ENOENT.
			 * Ported from upstream PR #15892
			 * (DDE_FLAG_FROM_FLUSHING handling).
			 */
			if (dde->dde_from_flushing) {
				VERIFY(ddt_log_remove_key(ddt,
				    ddt->ddt_log_flushing, &ddlwe.ddlwe_key));
			}

			/*
			 * Stamp the entry with the time of this modification.
			 * Without this an entry keeps the class_start it was
			 * loaded with, so one that has only just dropped back
			 * to the unique class looks as old as the entry's
			 * first write and is immediately eligible for an age-
			 * or percentage-based prune.  Matches upstream OpenZFS.
			 */
			if (ddt->ddt_flags & DDT_FLAG_FLAT) {
				ddlwe.ddlwe_phys.ddp_flat.ddp_class_start =
				    ddt_class_start();
			}

			ddt_log_entry(ddt, &ddlwe, &dlu);
			ddt_sync_scan_entry(ddt, &ddlwe, tx);
			ddt_free(ddt, dde);
		}

		ddt_log_commit(ddt, &dlu);

		/*
		 * Sync the stats for the store objects.  Even though we
		 * haven't modified anything on those objects, they're no
		 * longer the source of truth for entries that are now in the
		 * log, and we need the on-disk counts to reflect that,
		 * otherwise we'll miscount later when importing.
		 */
		for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
			for (ddt_class_t class = 0;
			    class < DDT_CLASSES; class++) {
				if (ddt_object_exists(ddt, type, class))
					ddt_object_sync(ddt, type, class, tx);
			}
		}

		bcopy(ddt->ddt_histogram, &ddt->ddt_histogram_cache,
		    sizeof (ddt->ddt_histogram));
	}

	if (spa_sync_pass(ddt->ddt_spa) == 1)
		/*
		 * Update ingest rate.  This is an exponential weighted moving
		 * average of the number of entries changed over recent txgs.
		 * The ramp-up cost shouldn't matter too much because the
		 * flusher will be trying to take at least the minimum anyway.
		 */
		ddt->ddt_log_ingest_rate = _ewma(
		    count, ddt->ddt_log_ingest_rate,
		    zfs_dedup_log_flush_flow_rate_txgs);
}

static void
ddt_sync_table(ddt_t *ddt, dmu_tx_t *tx, uint64_t txg)
{
	spa_t *spa = ddt->ddt_spa;
	ddt_entry_t *dde;
	void *cookie = NULL;

	if (avl_numnodes(&ddt->ddt_tree) == 0)
		return;

	ASSERT(spa->spa_uberblock.ub_version >= SPA_VERSION_DEDUP);

	if (spa->spa_ddt_stat_object == 0) {
		spa->spa_ddt_stat_object = zap_create_link(ddt->ddt_os,
		    DMU_OT_DDT_STATS, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_DDT_STATS, tx);
	}

	/* First-time touch on this DDT: pick a layout. */
	if (ddt->ddt_version == DDT_VERSION_UNCONFIGURED)
		VERIFY0(ddt_configure(ddt, B_TRUE));

	/* First write into an FDT DDT: materialize the directory ZAP. */
	if (ddt->ddt_version == DDT_VERSION_FDT && ddt->ddt_dir_object == 0)
		ddt_create_dir(ddt, tx);

	if (ddt->ddt_flags & DDT_FLAG_LOG) {
		/* Log active: append this txg's changes to the log. */
		ddt_sync_table_log(ddt, tx);
	} else {
		/* No log: drain the in-memory tree straight to the store. */
		while ((dde =
		    avl_destroy_nodes(&ddt->ddt_tree, &cookie)) != NULL) {
			/* Stamp the modification; see ddt_sync_table_log(). */
			if (ddt->ddt_flags & DDT_FLAG_FLAT) {
				dde->dde_phys->ddp_flat.ddp_class_start =
				    ddt_class_start();
			}
			ddt_sync_entry(ddt, dde, tx, txg);
			ddt_free(ddt, dde);
		}

		ddt_sync_update_stats(ddt, tx);
	}
}

void
ddt_sync(spa_t *spa, uint64_t txg)
{
	dsl_scan_t *scn = spa->spa_dsl_pool->dp_scan;
	dmu_tx_t *tx;
	zio_t *rio;

	ASSERT(spa_syncing_txg(spa) == txg);

	tx = dmu_tx_create_assigned(spa->spa_dsl_pool, txg);

	rio = zio_root(spa, NULL, NULL,
	    ZIO_FLAG_CANFAIL | ZIO_FLAG_SPECULATIVE | ZIO_FLAG_SELF_HEAL);

	/*
	 * This function may cause an immediate scan of ddt blocks (see
	 * the comment above dsl_scan_ddt() for details). We set the
	 * scan's root zio here so that we can wait for any scan IOs in
	 * addition to the regular ddt IOs.
	 */
	ASSERT3P(scn->scn_zio_root, ==, NULL);
	scn->scn_zio_root = rio;

	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		ddt_t *ddt = spa->spa_ddt[c];
		if (ddt == NULL)
			continue;
		ddt_sync_table(ddt, tx, txg);
		if (ddt->ddt_flags & DDT_FLAG_LOG)
			ddt_sync_flush_log(ddt, tx);
		ddt_repair_table(ddt, rio);
	}

	(void) zio_wait(rio);
	scn->scn_zio_root = NULL;

	/*
	 * Table and log objects may have been dirtied this txg (entry
	 * sync, log append/flush, truncation).  Invalidate the cached
	 * dedup table size so the next reader resamples the objects
	 * (see ddt_get_ddt_dsize()).
	 */
	spa->spa_dedup_dsize = ~0ULL;

	dmu_tx_commit(tx);
}

/*
 * ==========================================================================
 * DDT pruning (zpool ddtprune)
 *
 * Ported from OpenZFS PR #16277 and adapted to this tree's DDT API: this
 * backport exposes the older ddt_walk(spa, ddb, ddt_entry_t *) rather than
 * ddt_walk_impl()/ddlwe, and tracks per-entry "loaded"/"logged" state as
 * discrete ddt_entry_t fields (dde_loaded / dde_logged) instead of a
 * dde_flags bitfield.  Only flat (FDT) tables carry the ddp_class_start
 * timestamp pruning needs; legacy/trad tables are skipped (no-op).
 * ==========================================================================
 */
typedef struct ddt_prune_entry {
	ddt_t		*dpe_ddt;
	ddt_key_t	dpe_key;
	list_node_t	dpe_node;
	ddt_univ_phys_t	dpe_phys[];
} ddt_prune_entry_t;

typedef struct ddt_prune_info {
	spa_t		*dpi_spa;
	uint64_t	dpi_txg_syncs;
	uint64_t	dpi_pruned;
	list_t		dpi_candidates;
} ddt_prune_info_t;

/*
 * Apply a batch of prune candidates in sync context (run via dsl_sync_task).
 * For each candidate still stale (not reloaded for update this txg, and not a
 * live log entry), zero its flat phys so ddt_sync neither frees its DVAs nor
 * reuses the entry.
 */
static void
prune_candidates_sync(void *arg, dmu_tx_t *tx)
{
	(void) tx;
	ddt_prune_info_t *dpi = arg;
	ddt_prune_entry_t *dpe;

	spa_config_enter(dpi->dpi_spa, SCL_ZIO, FTAG, RW_READER);

	/* Process the prune candidates collected so far */
	while ((dpe = list_remove_head(&dpi->dpi_candidates)) != NULL) {
		blkptr_t blk;
		ddt_t *ddt = dpe->dpe_ddt;

		ddt_enter(ddt);

		/*
		 * If it's on the live tree, then it was loaded for update
		 * this txg and is no longer stale; skip it.
		 */
		if (avl_find(&ddt->ddt_tree, &dpe->dpe_key, NULL)) {
			ddt_exit(ddt);
			kmem_free(dpe, sizeof (ddt_prune_entry_t) +
			    DDT_FLAT_PHYS_SIZE);
			continue;
		}

		ddt_bp_create(ddt->ddt_checksum, &dpe->dpe_key,
		    dpe->dpe_phys, DDT_PHYS_FLAT, &blk);

		ddt_entry_t *dde = ddt_lookup_unique(ddt, &blk);
		/*
		 * Prune only an entry that is exactly what the walk read.  The
		 * walk reads the stored unique class, which lags the dedup log;
		 * if the log flushed since, a candidate that gained a reference
		 * is now found in the DUPLICATE class (ddt_lookup_unique() only
		 * tries UNIQUE first, then falls back to every class), is not
		 * dde_logged, and would be cleared -- freeing a block that is
		 * still referenced.  Any added reference changes the phys.
		 * Adapted from upstream OpenZFS PR #19223.
		 */
		if (dde != NULL && !dde->dde_logged &&
		    memcmp(dde->dde_phys, dpe->dpe_phys,
		    DDT_FLAT_PHYS_SIZE) == 0) {
			ASSERT(dde->dde_loaded);
			/*
			 * Zero the physical, so we don't try to free DVAs
			 * at flush nor try to reuse this entry.
			 */
			ddt_phys_clear(dde->dde_phys, DDT_PHYS_FLAT);

			dpi->dpi_pruned++;
		}

		ddt_exit(ddt);
		kmem_free(dpe, sizeof (ddt_prune_entry_t) +
		    DDT_FLAT_PHYS_SIZE);
	}

	spa_config_exit(dpi->dpi_spa, SCL_ZIO, FTAG);
	dpi->dpi_txg_syncs++;
}

/*
 * Queue a stale unique entry to be pruned in sync context.  Collected in open
 * context during the walk; applied by prune_candidates_sync().
 */
static void
ddt_prune_entry(list_t *list, ddt_t *ddt, const ddt_key_t *ddk,
    const ddt_univ_phys_t *ddp)
{
	ASSERT(ddt->ddt_flags & DDT_FLAG_FLAT);

	size_t dpe_size = sizeof (ddt_prune_entry_t) + DDT_FLAT_PHYS_SIZE;
	ddt_prune_entry_t *dpe = kmem_alloc(dpe_size, KM_SLEEP);

	dpe->dpe_ddt = ddt;
	dpe->dpe_key = *ddk;
	memcpy(dpe->dpe_phys, ddp, DDT_FLAT_PHYS_SIZE);
	list_insert_head(list, dpe);
}

/*
 * Iterate over all entries in the DDT unique class, doing one of:
 *  (a) build an age histogram (histogram != NULL), or
 *  (b) prune entries whose class_start is older than cutoff (cutoff != 0).
 *
 * Starting the bookmark at DDT_CLASS_UNIQUE restricts ddt_walk() to the
 * unique class.  The walk reads the stored entries only, which lag the
 * log; prune_candidates_sync() rechecks each candidate against the live,
 * logged and reloaded entry before pruning it.  Also called by zdb(8) to
 * dump the age histogram.
 */
void
ddt_prune_walk(spa_t *spa, uint64_t cutoff, ddt_age_histo_t *histogram)
{
	ddt_bookmark_t ddb = {
		.ddb_class = DDT_CLASS_UNIQUE,
		.ddb_type = 0,
		.ddb_checksum = 0,
		.ddb_cursor = 0
	};
	int error;
	int total = 0;
	uint64_t valid = 0;
	uint64_t candidates = 0;
	uint64_t now = gethrestime_sec();
	ddt_prune_info_t dpi;
	boolean_t pruning = (cutoff != 0);

	/*
	 * Stack-allocate a flat-sized ddt_entry_t for ddt_walk() to fill,
	 * mirroring how zdb walks the table.
	 */
	uint64_t ddebuf[DDT_ENTRY_BUF_WORDS] = { 0 };
	ddt_entry_t *dde = (ddt_entry_t *)ddebuf;

	if (pruning) {
		dpi.dpi_txg_syncs = 0;
		dpi.dpi_pruned = 0;
		dpi.dpi_spa = spa;
		list_create(&dpi.dpi_candidates, sizeof (ddt_prune_entry_t),
		    offsetof(ddt_prune_entry_t, dpe_node));
	}

	if (histogram != NULL)
		memset(histogram, 0, sizeof (ddt_age_histo_t));

	/*
	 * ddt_walk() below opens a non-prefetching ZAP cursor, so cold
	 * unique-class leaves would otherwise be read one small random I/O
	 * at a time.  Issue a whole-object async prefetch of each
	 * unique-class store up front so the walk finds them warm.
	 */
	if (zfs_ddt_prune_prefetch) {
		for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
			ddt_t *ddt = spa->spa_ddt[c];
			if (ddt == NULL)
				continue;
			for (ddt_type_t type = 0; type < DDT_TYPES; type++)
				ddt_object_prefetch_all(ddt, type,
				    DDT_CLASS_UNIQUE);
		}
	}

	while ((error = ddt_walk(spa, &ddb, dde)) == 0) {
		ddt_t *ddt = spa->spa_ddt[ddb.ddb_checksum];
		VERIFY(ddt);

		if (spa_shutting_down(spa) || issig(JUSTLOOKING))
			break;

		/* Only flat (FDT) tables carry the class_start timestamp. */
		if (!(ddt->ddt_flags & DDT_FLAG_FLAT))
			continue;

		total++;

		ASSERT3U(dde->dde_phys->ddp_flat.ddp_refcnt, <=, 1);

		uint64_t class_start = dde->dde_phys->ddp_flat.ddp_class_start;

		/*
		 * Prune older entries.  A zero class_start means the entry
		 * predates ddp_class_start tracking, so it has no recorded
		 * age; treat it as infinitely old, matching upstream.  Any
		 * cutoff is then above it, so "zpool ddtprune -p 100" clears
		 * such entries instead of stranding them permanently.
		 */
		if (pruning && class_start < cutoff) {
			if (candidates++ >= zfs_ddt_prunes_per_txg) {
				/* sync prune candidates in batches */
				VERIFY0(dsl_sync_task(spa_name(spa),
				    NULL, prune_candidates_sync,
				    &dpi, 0, ZFS_SPACE_CHECK_NONE));
				candidates = 1;
			}
			ddt_prune_entry(&dpi.dpi_candidates, ddt,
			    &dde->dde_key, dde->dde_phys);
		}

		/* build a histogram */
		if (histogram != NULL) {
			uint64_t age = (class_start < now) ?
			    (now - class_start) / 3600 : 0;
			int bin = MIN(highbit64(age), HIST_BINS - 1);
			histogram->dah_entries++;
			histogram->dah_age_histo[bin]++;
		}

		valid++;
	}

	if (pruning) {
		if (!list_is_empty(&dpi.dpi_candidates)) {
			/* sync out final batch of prune candidates */
			VERIFY0(dsl_sync_task(spa_name(spa), NULL,
			    prune_candidates_sync, &dpi, 0,
			    ZFS_SPACE_CHECK_NONE));
		}
		list_destroy(&dpi.dpi_candidates);

		if (valid > 0) {
			zfs_dbgmsg("pruned %llu entries (%llu%%) across %llu "
			    "txg syncs", (u_longlong_t)dpi.dpi_pruned,
			    (u_longlong_t)((dpi.dpi_pruned * 100) / valid),
			    (u_longlong_t)dpi.dpi_txg_syncs);
		}
	}
}

static uint64_t
ddt_total_entries(spa_t *spa)
{
	ddt_object_t ddo;
	ddt_get_dedup_object_stats(spa, &ddo);

	return (ddo.ddo_count);
}

/* spa_active_ddt_prune is claimed with atomic_cas_uint() below. */
CTASSERT(sizeof (boolean_t) == sizeof (uint_t));

int
ddt_prune_unique_entries(spa_t *spa, zpool_ddt_prune_unit_t unit,
    uint64_t amount)
{
	uint64_t cutoff;
	uint64_t start_time = gethrtime();
	uint64_t now = gethrestime_sec();

	/*
	 * Validate the request before claiming the prune, so that a rejected
	 * request can never leave the prune marked active.  The percentage
	 * range matches what zpool(8) enforces; through lzc_ddt_prune(), 0%
	 * made the cutoff the current time and pruned nearly every unique
	 * entry.
	 */
	switch (unit) {
	case ZPOOL_DDT_PRUNE_PERCENTAGE:
		if (amount == 0 || amount > 100)
			return (SET_ERROR(EINVAL));
		break;
	case ZPOOL_DDT_PRUNE_AGE:
		if (amount >= now)
			return (SET_ERROR(EINVAL));
		break;
	default:
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Claim the prune atomically, before any other work.  Testing the
	 * mark and setting it later let two concurrent prunes both proceed;
	 * the first to finish then cleared the mark while the other was still
	 * pruning, and that prune's I/O lost the SCL_ZIO priority that
	 * zio_vdev_io_start() gives an active prune.  From here on, every
	 * exit must release the claim.
	 */
	if (atomic_cas_uint((volatile uint_t *)&spa->spa_active_ddt_prune,
	    B_FALSE, B_TRUE) != B_FALSE)
		return (SET_ERROR(EALREADY));

	if (ddt_total_entries(spa) == 0) {
		spa->spa_active_ddt_prune = B_FALSE;
		return (0);
	}

	zfs_dbgmsg("prune %llu %s", (u_longlong_t)amount,
	    unit == ZPOOL_DDT_PRUNE_PERCENTAGE ? "%" : "seconds old or older");

	if (unit == ZPOOL_DDT_PRUNE_PERCENTAGE) {
		ddt_age_histo_t histogram;
		uint64_t oldest = 0;

		/* Make a pass over the DDT to build a histogram */
		ddt_prune_walk(spa, 0, &histogram);

		uint64_t target = (histogram.dah_entries * amount) / 100;

		/*
		 * The target rounds down to whole entries.  If it is zero --
		 * e.g. 1% of fewer than 100 unique entries -- the bin search
		 * below never runs, oldest stays 0, the cutoff becomes the
		 * current time, and the walk prunes every unique entry
		 * instead of none.  Prune nothing.  (Upstream OpenZFS #19264.)
		 */
		if (target == 0) {
			zfs_dbgmsg("%s: prune target rounds to 0 entries",
			    spa_name(spa));
			spa->spa_active_ddt_prune = B_FALSE;
			return (0);
		}

		/*
		 * Figure out our cutoff date (i.e., which bins to prune from).
		 */
		for (int i = HIST_BINS - 1; i >= 0 && target > 0; i--) {
			if (histogram.dah_age_histo[i] != 0) {
				if (target <= histogram.dah_age_histo[i]) {
					oldest = (i == 0) ? 0 :
					    (1ULL << (i - 1)) * 3600;
					target = 0;
				} else {
					target -= histogram.dah_age_histo[i];
				}
			}
		}
		cutoff = now - oldest;
	} else {
		/* ZPOOL_DDT_PRUNE_AGE; validated above */
		cutoff = now - amount;
	}

	if (cutoff > 0 && !spa_shutting_down(spa) && !issig(JUSTLOOKING)) {
		/* Traverse the DDT to prune entries older than our cutoff */
		ddt_prune_walk(spa, cutoff, NULL);
	}

	zfs_dbgmsg("%s: prune completed in %llu ms", spa_name(spa),
	    (u_longlong_t)NSEC2MSEC(gethrtime() - start_time));

	spa->spa_active_ddt_prune = B_FALSE;
	return (0);
}

int
ddt_walk(spa_t *spa, ddt_bookmark_t *ddb, ddt_entry_t *dde)
{
	do {
		do {
			do {
				ddt_t *ddt = spa->spa_ddt[ddb->ddb_checksum];
				int error = ENOENT;
				if (ddt_object_exists(ddt, ddb->ddb_type,
				    ddb->ddb_class)) {
					error = ddt_object_walk(ddt,
					    ddb->ddb_type, ddb->ddb_class,
					    &ddb->ddb_cursor, dde);
				}
				dde->dde_type = ddb->ddb_type;
				dde->dde_class = ddb->ddb_class;
				if (error == 0)
					return (0);
				if (error != ENOENT)
					return (error);
				ddb->ddb_cursor = 0;
			} while (++ddb->ddb_checksum < ZIO_CHECKSUM_FUNCTIONS);
			ddb->ddb_checksum = 0;
		} while (++ddb->ddb_type < DDT_TYPES);
		ddb->ddb_type = 0;
	} while (++ddb->ddb_class < DDT_CLASSES);

	return (SET_ERROR(ENOENT));
}

/*
 * ==========================================================================
 * Fast Dedup (FDT) on-disk layout
 *
 * Legacy DDT objects live as children of the MOS root, named
 * "DDT-<cksum>-<type>-<class>".  An FDT DDT lives inside a per-checksum
 * ZAP in the MOS root named "DDT-<cksum>"; that ZAP holds "version" and
 * "flags" entries plus the type/class objects (named "<type>-<class>"
 * without the DDT- prefix, since the checksum is implicit in the parent).
 *
 * On a pool with feature@fast_dedup disabled, every DDT is configured as
 * DDT_VERSION_LEGACY and on-disk behaviour is unchanged.  On a pool with
 * the feature enabled, a *new* DDT (one that has never been written
 * before) is configured as DDT_VERSION_FDT and gets its directory ZAP
 * lazily, on the first sync that needs to write to it.  Existing legacy
 * DDTs stay legacy; the migration is per-checksum and gradual.
 *
 * ddt_flags is always zero in this patch; FLAT/LOG behaviour layers on
 * in later patches.
 * ==========================================================================
 */

/*
 * Default ddt_flags for each version.  Indexed by ddt_version_t.
 *   [DDT_VERSION_LEGACY] = 0     -- no flags possible
 *   [DDT_VERSION_FDT]    = 0     -- placeholder; later patches add
 *                                   DDT_FLAG_FLAT | DDT_FLAG_LOG here
 */
const uint64_t ddt_version_flags[] = {
	[DDT_VERSION_LEGACY] = 0,
	[DDT_VERSION_FDT]    = DDT_FLAG_FLAT | DDT_FLAG_LOG,
};

/* Create the per-DDT directory ZAP and account the feature as active. */
void
ddt_create_dir(ddt_t *ddt, dmu_tx_t *tx)
{
	ASSERT0(ddt->ddt_dir_object);
	ASSERT3U(ddt->ddt_version, ==, DDT_VERSION_FDT);

	char name[DDT_NAMELEN];
	(void) snprintf(name, sizeof (name), DMU_POOL_DDT_DIR,
	    zio_checksum_table[ddt->ddt_checksum].ci_name);

	/*
	 * Plain metadata ZAP, as upstream: zdb's leak detector knows
	 * about ddt_dir_object explicitly (see dump_mos_leaks), and
	 * DMU_OT_DDT_ZAP would make zdb suppress the dir's name
	 * entries in -dddd dumps (dump_ddt_zap prints stats only).
	 */
	ddt->ddt_dir_object = zap_create_link(ddt->ddt_os,
	    DMU_OTN_ZAP_METADATA, DMU_POOL_DIRECTORY_OBJECT, name, tx);

	VERIFY0(zap_add(ddt->ddt_os, ddt->ddt_dir_object, DDT_DIR_VERSION,
	    sizeof (uint64_t), 1, &ddt->ddt_version, tx));
	VERIFY0(zap_add(ddt->ddt_os, ddt->ddt_dir_object, DDT_DIR_FLAGS,
	    sizeof (uint64_t), 1, &ddt->ddt_flags, tx));

	spa_feature_incr(ddt->ddt_spa, SPA_FEATURE_FAST_DEDUP, tx);

	ddt_dbg("ddt_create_dir: cksum=%u dir_object=%llu name=%s txg=%llu",
	    (uint_t)ddt->ddt_checksum,
	    (u_longlong_t)ddt->ddt_dir_object, name,
	    (u_longlong_t)dmu_tx_get_txg(tx));
}

/*
 * Tear down the per-DDT directory ZAP and account the feature as no
 * longer active for this DDT.
 */
void
ddt_destroy_dir(ddt_t *ddt, dmu_tx_t *tx)
{
	ASSERT3U(ddt->ddt_dir_object, !=, 0);
	ASSERT3U(ddt->ddt_dir_object, !=, DMU_POOL_DIRECTORY_OBJECT);
	ASSERT3U(ddt->ddt_version, ==, DDT_VERSION_FDT);

	char name[DDT_NAMELEN];
	(void) snprintf(name, sizeof (name), DMU_POOL_DDT_DIR,
	    zio_checksum_table[ddt->ddt_checksum].ci_name);

	for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
		for (ddt_class_t class = 0; class < DDT_CLASSES; class++) {
			ASSERT(!ddt_object_exists(ddt, type, class));
		}
	}

	/*
	 * If this DDT has a log, the dir ZAP also holds two entries
	 * referencing the active and flushing log objects.  Tear them
	 * down before we check the directory count, otherwise we see
	 * 4 instead of the expected 2 and the ASSERT3U below fires.
	 */
	if (ddt->ddt_flags & DDT_FLAG_LOG)
		ddt_log_destroy(ddt, tx);

	uint64_t count;
	VERIFY0(zap_count(ddt->ddt_os, ddt->ddt_dir_object, &count));
	/* Only the two metadata entries (version, flags) should remain. */
	ASSERT3U(count, ==, 2);

	VERIFY0(zap_remove(ddt->ddt_os, DMU_POOL_DIRECTORY_OBJECT, name, tx));
	uint64_t freed_dir = ddt->ddt_dir_object;
	VERIFY0(zap_destroy(ddt->ddt_os, ddt->ddt_dir_object, tx));

	ddt->ddt_dir_object = 0;

	spa_feature_decr(ddt->ddt_spa, SPA_FEATURE_FAST_DEDUP, tx);

	ddt_dbg("ddt_destroy_dir: cksum=%u dir_object=%llu name=%s txg=%llu",
	    (uint_t)ddt->ddt_checksum,
	    (u_longlong_t)freed_dir, name,
	    (u_longlong_t)dmu_tx_get_txg(tx));
}

/*
 * Determine version, flags and on-disk layout for this DDT.
 *
 * Inspection order:
 *   1. If feature@fast_dedup is active and the per-DDT directory ZAP
 *      exists, load version+flags from it (FDT).
 *   2. Otherwise, if any legacy DDT object exists in the MOS root for this
 *      checksum, this is a legacy DDT (LEGACY).
 *   3. Otherwise (no objects yet): if `new` is false return ENOENT (caller
 *      treats as "no DDT yet, defer"); if `new` is true pick a layout
 *      based on whether the feature is enabled (FDT) or not (LEGACY).
 */
int
ddt_configure(ddt_t *ddt, boolean_t new)
{
	spa_t *spa = ddt->ddt_spa;
	char name[DDT_NAMELEN];
	int error;

	boolean_t fdt_enabled =
	    spa_feature_is_enabled(spa, SPA_FEATURE_FAST_DEDUP);
	boolean_t fdt_active =
	    spa_feature_is_active(spa, SPA_FEATURE_FAST_DEDUP);
	if (fdt_active) {
		(void) snprintf(name, sizeof (name), DMU_POOL_DDT_DIR,
		    zio_checksum_table[ddt->ddt_checksum].ci_name);

		error = zap_lookup(spa->spa_meta_objset,
		    DMU_POOL_DIRECTORY_OBJECT, name, sizeof (uint64_t), 1,
		    &ddt->ddt_dir_object);
		if (error == 0) {
			ASSERT3P(spa->spa_meta_objset, ==, ddt->ddt_os);

			error = zap_lookup(ddt->ddt_os, ddt->ddt_dir_object,
			    DDT_DIR_VERSION, sizeof (uint64_t), 1,
			    &ddt->ddt_version);
			if (error != 0)
				return (error);

			error = zap_lookup(ddt->ddt_os, ddt->ddt_dir_object,
			    DDT_DIR_FLAGS, sizeof (uint64_t), 1,
			    &ddt->ddt_flags);
			if (error != 0)
				return (error);

			if (ddt->ddt_version != DDT_VERSION_FDT) {
				cmn_err(CE_WARN, "ddt_configure: spa=%s "
				    "ddt_dir=%s unknown version %llu",
				    spa_name(spa), name,
				    (u_longlong_t)ddt->ddt_version);
				return (SET_ERROR(EINVAL));
			}

			if ((ddt->ddt_flags & ~DDT_FLAG_MASK) != 0) {
				cmn_err(CE_WARN, "ddt_configure: spa=%s "
				    "ddt_dir=%s version=%llu unknown flags "
				    "%llx", spa_name(spa), name,
				    (u_longlong_t)ddt->ddt_version,
				    (u_longlong_t)ddt->ddt_flags);
				return (SET_ERROR(EINVAL));
			}

			return (0);
		}
		if (error != ENOENT)
			return (error);
	}

	/*
	 * No FDT directory.  Look for a legacy object in the MOS root for
	 * any (type,class); if any is found, this is a legacy DDT.
	 */
	ddt->ddt_version = DDT_VERSION_LEGACY;
	ddt->ddt_flags = ddt_version_flags[DDT_VERSION_LEGACY];
	ddt->ddt_dir_object = DMU_POOL_DIRECTORY_OBJECT;

	for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
		for (ddt_class_t class = 0; class < DDT_CLASSES; class++) {
			ddt_object_name(ddt, type, class, name);
			uint64_t obj;
			error = zap_lookup(spa->spa_meta_objset,
			    DMU_POOL_DIRECTORY_OBJECT, name,
			    sizeof (uint64_t), 1, &obj);
			if (error == ENOENT)
				continue;
			if (error != 0)
				return (error);
			/* Found one -- legacy layout is confirmed. */
			return (0);
		}
	}

	/* No on-disk DDT at all. */
	if (!new) {
		/* Caller is loading; nothing to configure. */
		ddt->ddt_version = DDT_VERSION_UNCONFIGURED;
		ddt->ddt_dir_object = 0;
		return (SET_ERROR(ENOENT));
	}

	/* Caller is about to write -- pick the best layout we can. */
	if (fdt_enabled) {
		ddt->ddt_version = DDT_VERSION_FDT;
		ddt->ddt_flags = ddt_version_flags[DDT_VERSION_FDT];
		ddt->ddt_dir_object = 0; /* materialised on first sync */
	} else {
		ddt->ddt_version = DDT_VERSION_LEGACY;
		ddt->ddt_flags = ddt_version_flags[DDT_VERSION_LEGACY];
		ddt->ddt_dir_object = DMU_POOL_DIRECTORY_OBJECT;
	}

	return (0);
}
