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
 * Copyright (c) 2016 by Delphix. All rights reserved.
 * Copyright 2026 Gernot Strasser
 */

#ifndef _SYS_DDT_H
#define	_SYS_DDT_H

#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/fs/zfs.h>
#include <sys/zio.h>
#include <sys/dmu.h>
#include <sys/avl.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct abd;

/*
 * On-disk DDT formats, in the desired search order (newest version first).
 */
typedef enum {
	DDT_TYPE_ZAP = 0,
	DDT_TYPES
} ddt_type_t;

/* Default (and currently only) on-disk DDT object type. */
#define	DDT_TYPE_DEFAULT	(DDT_TYPE_ZAP)

/*
 * DDT classes, in the desired search order (highest replication level first).
 */
typedef enum {
	DDT_CLASS_DITTO = 0,
	DDT_CLASS_DUPLICATE,
	DDT_CLASS_UNIQUE,
	DDT_CLASSES
} ddt_class_t;

/*
 * On-disk ddt entry:  key (name) and physical storage (value).
 */
typedef struct ddt_key {
	zio_cksum_t	ddk_cksum;	/* 256-bit block checksum */
	/* BEGIN CSTYLED */
	/*
	 * Encoded with logical & physical size, encryption, and compression,
	 * as follows:
	 *   +-------+-------+-------+-------+-------+-------+-------+-------+
	 *   |   0   |   0   |   0   |X| comp|     PSIZE     |     LSIZE     |
	 *   +-------+-------+-------+-------+-------+-------+-------+-------+
	 */
	/* END CSTYLED */
	uint64_t	ddk_prop;
} ddt_key_t;

#define	DDK_GET_LSIZE(ddk)	\
	BF64_GET_SB((ddk)->ddk_prop, 0, 16, SPA_MINBLOCKSHIFT, 1)
#define	DDK_SET_LSIZE(ddk, x)	\
	BF64_SET_SB((ddk)->ddk_prop, 0, 16, SPA_MINBLOCKSHIFT, 1, x)

#define	DDK_GET_PSIZE(ddk)	\
	BF64_GET_SB((ddk)->ddk_prop, 16, 16, SPA_MINBLOCKSHIFT, 1)
#define	DDK_SET_PSIZE(ddk, x)	\
	BF64_SET_SB((ddk)->ddk_prop, 16, 16, SPA_MINBLOCKSHIFT, 1, x)

#define	DDK_GET_COMPRESS(ddk)		BF64_GET((ddk)->ddk_prop, 32, 7)
#define	DDK_SET_COMPRESS(ddk, x)	BF64_SET((ddk)->ddk_prop, 32, 7, x)

#define	DDK_GET_CRYPT(ddk)		BF64_GET((ddk)->ddk_prop, 39, 1)
#define	DDK_SET_CRYPT(ddk, x)	BF64_SET((ddk)->ddk_prop, 39, 1, x)

#define	DDT_KEY_WORDS	(sizeof (ddt_key_t) / sizeof (uint64_t))

#define	DDE_GET_NDVAS(dde) (DDK_GET_CRYPT(&dde->dde_key) \
	? SPA_DVAS_PER_BP - 1 : SPA_DVAS_PER_BP)

/*
 * One "slot" of a traditional dedup phys.  The traditional DDT entry
 * carries DDT_PHYS_MAX of these in ddp_trad, one per copy count
 * (DITTO / SINGLE / DOUBLE / TRIPLE).
 */
typedef struct ddt_phys {
	dva_t		ddp_dva[SPA_DVAS_PER_BP];
	uint64_t	ddp_refcnt;
	uint64_t	ddp_phys_birth;
} ddt_phys_t;

/* Number of trad phys slots in a ddt_univ_phys_t. */
#define	DDT_PHYS_MAX	(4)

/*
 * Universal phys.  Allocated at variable size depending on layout:
 *
 *   trad (legacy / FDT-without-flat): DDT_PHYS_MAX entries of ddp_trad
 *   flat (FDT with DDT_FLAG_FLAT):    single ddp_flat slot, smaller and
 *                                     with an extra age field for future
 *                                     DDT pruning.
 *
 * Backed by a flexible array on struct ddt_entry::dde_phys; the
 * allocator (ddt_alloc) chooses trad-size or flat-size based on
 * ddt->ddt_flags & DDT_FLAG_FLAT.  Consumers MUST go through
 * DDT_PHYS_VARIANT() to pick the right union member, and MUST NOT
 * touch a member the layout does not provide (e.g. ddp_trad on a
 * flat-sized allocation).
 */
typedef union {
	/* Traditional layout: DDT_PHYS_MAX slots of the named ddt_phys_t. */
	struct ddt_phys	ddp_trad[DDT_PHYS_MAX];
	/*
	 * Flat layout: single slot with an extra ddp_class_start field.
	 * The first three fields share offsets with struct ddt_phys, so
	 * code can safely cast (ddt_phys_t *)&ddp_flat to read or write
	 * dva/refcnt/birth.  Don't access ddp_class_start through such a
	 * cast.
	 */
	struct {
		dva_t		ddp_dva[SPA_DVAS_PER_BP];
		uint64_t	ddp_refcnt;
		uint64_t	ddp_phys_birth;
		uint64_t	ddp_class_start; /* for DDT pruning */
	} ddp_flat;
} ddt_univ_phys_t;

typedef enum {
	DDT_PHYS_DITTO = 0,
	DDT_PHYS_SINGLE = 1,
	DDT_PHYS_DOUBLE = 2,
	DDT_PHYS_TRIPLE = 3,
	DDT_PHYS_FLAT = 4,
	DDT_PHYS_NONE = 5
} ddt_phys_variant_t;

/*
 * Loop bound through the traditional trad slots.  In flat mode the trad
 * array is not present; consumers must consult ddt_flags before iterating
 * past slot 0.  Equals DDT_PHYS_MAX.
 */
#define	DDT_PHYS_TYPES	DDT_PHYS_MAX

/*
 * Dispatch a variant index for a given ddt.  If the table is flat, the
 * answer is always DDT_PHYS_FLAT regardless of the requested slot.  In
 * trad mode the caller's slot is returned unchanged.
 */
#define	DDT_PHYS_VARIANT(ddt, p)	\
	(((ddt)->ddt_flags & DDT_FLAG_FLAT) ? DDT_PHYS_FLAT : (p))

/*
 * For array fields indexed per-phys (e.g. dde_lead_zio[]) -- flat tables
 * always use slot 0, trad uses the caller's slot.
 */
#define	DDT_PHYS_FOR_COPIES(ddt, p)	\
	(((ddt)->ddt_flags & DDT_FLAG_FLAT) ? 0 : (p))

/* Number of phys slots actually in use for this ddt. */
#define	DDT_NPHYS(ddt)	\
	(((ddt)->ddt_flags & DDT_FLAG_FLAT) ? 1 : DDT_PHYS_MAX)

/*
 * Whether phys slot p is an (obsolete) ditto slot.  Flat tables have no
 * ditto slot; trad tables use slot 0 (DDT_PHYS_DITTO) for ditto.
 */
#define	DDT_PHYS_IS_DITTO(ddt, p)	_DDT_PHYS_SWITCH(ddt, 0, (p == 0))

/* Size of the on-disk / in-core phys blob for this ddt. */
#define	DDT_TRAD_PHYS_SIZE	sizeof (((ddt_univ_phys_t *)0)->ddp_trad)
#define	DDT_FLAT_PHYS_SIZE	sizeof (((ddt_univ_phys_t *)0)->ddp_flat)
#define	_DDT_PHYS_SWITCH(ddt, flat, trad)	\
	(((ddt)->ddt_flags & DDT_FLAG_FLAT) ? (flat) : (trad))
#define	DDT_PHYS_SIZE(ddt)	\
	_DDT_PHYS_SWITCH(ddt, DDT_FLAT_PHYS_SIZE, DDT_TRAD_PHYS_SIZE)

/* Max of trad and flat phys size; useful for static stack buffers. */
#define	DDT_PHYS_SIZE_MAX	DDT_TRAD_PHYS_SIZE

/*
 * Return a (ddt_phys_t *) pointing at the right phys slot for variant v.
 * For DDT_PHYS_FLAT the cast is valid: the trad-slot struct (dva, refcnt,
 * birth) is a prefix of the flat struct (dva, refcnt, birth, class_start),
 * so reading/writing those three fields via a ddt_phys_t * is well-defined.
 * Do NOT touch class_start through this pointer.
 */
#define	DDT_PHYS_SLOT(ddp, v)	\
	((v) == DDT_PHYS_FLAT ? (ddt_phys_t *)&(ddp)->ddp_flat : \
	    &(ddp)->ddp_trad[v])

/*
 * DDT-wide feature flags.  Stored in ddt_flags; placeholder for the
 * Fast Dedup work to come.  Always zero until later patches activate
 * them.
 */
#define	DDT_FLAG_FLAT	(1 << 0)	/* single extensible phys */
#define	DDT_FLAG_LOG	(1 << 1)	/* dedup log (journal) */
#define	DDT_FLAG_MASK	(DDT_FLAG_FLAT|DDT_FLAG_LOG)

/* DDT version numbers */
#define	DDT_VERSION_LEGACY	(0)
#define	DDT_VERSION_FDT		(1)

/*
 * In-core and on-disk bookmark for DDT walks
 */
typedef struct ddt_bookmark {
	uint64_t	ddb_class;
	uint64_t	ddb_type;
	uint64_t	ddb_checksum;
	uint64_t	ddb_cursor;
} ddt_bookmark_t;

/*
 * Age histogram used by DDT pruning (zpool ddtprune) and zdb -D.  Each bin
 * is a power-of-two range of entry ages in hours; dah_entries is the total
 * number of unique-class entries seen.  See ddt_prune_walk() in ddt.c.
 */
#define	HIST_BINS	16
typedef struct ddt_age_histo {
	uint64_t	dah_entries;
	uint64_t	dah_age_histo[HIST_BINS];
} ddt_age_histo_t;

/*
 * Per-entry I/O state.  Lazy-allocated by ddt_io_alloc() when first
 * needed (typically just before a dedup write completes), freed by
 * ddt_io_free() in ddt_free().
 *
 * dde_io_lock protects dde_lead_zio[] and dde_repair_abd during
 * concurrent I/O.  Without this lock, two completions on the same
 * entry can race -- which under stress (ztest etc) produces
 * intermittent checksum errors and orphaned in-flight zios.
 *
 * This substruct also holds dde_orig_phys (below) for I/O-failure
 * rollback: a snapshot of dde_phys taken just before a dedup write
 * is queued, so a failed write can be reverted via ddt_phys_unextend()
 * instead of leaving dde_phys partially-modified.
 */
typedef struct ddt_entry_io {
	kmutex_t	dde_io_lock;
	abd_t		*dde_repair_abd;
	zio_t		*dde_lead_zio[DDT_PHYS_MAX];
	/*
	 * Snapshot of dde_phys taken just before a dedup write is
	 * queued.  If that write fails in completion, we revert
	 * dde_phys back to this state via ddt_phys_unextend().
	 * Without this, a failed dedup write leaves dde_phys
	 * partially-modified -- subsequent readers of the same entry
	 * see wrong DVAs and get ECKSUM.
	 */
	ddt_univ_phys_t	dde_orig_phys;
} ddt_entry_io_t;

/*
 * In-core ddt entry.
 *
 * Defined here (rather than in <sys/ddt_impl.h>) because dsl_scan,
 * zio, and zdb stack-allocate and dereference instances of this type.
 */
struct ddt_entry {
	ddt_key_t	dde_key;
	avl_node_t	dde_node;
	ddt_type_t	dde_type;
	ddt_class_t	dde_class;
	uint8_t		dde_loading;
	uint8_t		dde_loaded;
	uint8_t		dde_overquota;	/* unusable: DDT over quota */
	uint8_t		dde_logged;	/* loaded from the DDT log */
	uint8_t		dde_from_flushing; /* rehydrated from flushing log */
	uint64_t	dde_waiters;	/* count of waiters on dde_cv */
	kcondvar_t	dde_cv;
	ddt_entry_io_t	*dde_io;	/* lazy I/O state; protects dde_phys */
	/*
	 * Back-pointer to the owning DDT.  Set by ddt_alloc(), used by the
	 * ZAP storage layer (which doesn't otherwise have ddt in scope) to
	 * size the on-disk phys blob via DDT_PHYS_SIZE(dde->dde_ddt).
	 */
	ddt_t		*dde_ddt;
	/*
	 * dde_phys is a flexible-array member.  Allocated size depends on
	 * ddt->ddt_flags & DDT_FLAG_FLAT: DDT_TRAD_PHYS_SIZE bytes for trad
	 * (where ddp_trad[0..DDT_PHYS_MAX-1] is valid), or DDT_FLAT_PHYS_SIZE
	 * bytes for flat (where only ddp_flat is valid).
	 */
	ddt_univ_phys_t	dde_phys[];
};

/*
 * A "lightweight" entry is a stack-friendly copy of an entry: the key, its
 * storage type/class, and a full (max-size) phys.  Used to pass entries
 * around (for example through the DDT log) without holding the live
 * ddt_entry_t.
 */
typedef struct {
	ddt_key_t	ddlwe_key;
	ddt_type_t	ddlwe_type;
	ddt_class_t	ddlwe_class;
	ddt_univ_phys_t	ddlwe_phys;
} ddt_lightweight_entry_t;

/*
 * In-core DDT log.  A separate struct to make it easier to switch between the
 * appending and flushing logs.  See ddt_log.c.
 */
typedef struct {
	avl_tree_t	ddl_tree;	/* logged entries */
	uint32_t	ddl_flags;	/* flags for this log */
	uint64_t	ddl_object;	/* log object id */
	uint64_t	ddl_length;	/* on-disk log size */
	uint64_t	ddl_first_txg;	/* txg log became active */
	ddt_key_t	ddl_checkpoint;	/* last checkpoint */
} ddt_log_t;

/*
 * In-core ddt.
 *
 * Defined here (rather than in <sys/ddt_impl.h>) because dsl_scan
 * and zio dereference ddt_tree and friends.
 */
struct ddt {
	kmutex_t	ddt_lock;
	avl_tree_t	ddt_tree;
	avl_tree_t	ddt_repair_tree;
	avl_tree_t	ddt_log_tree;	/* in-core mirror of active log */

	/*
	 * Protects ddt_object[] against open-context readers (prefetch,
	 * walk, count, repair lookup) racing sync-context
	 * ddt_object_destroy().  Ported from upstream 765929cb4.
	 */
	krwlock_t	ddt_objects_lock;
	enum zio_checksum ddt_checksum;
	uint64_t	ddt_version;	/* LEGACY or FDT */
	uint64_t	ddt_flags;	/* DDT_FLAG_*; currently 0 */
	uint64_t	ddt_dir_object;	/* MOS dir for FDT (0 = legacy) */
	spa_t		*ddt_spa;
	objset_t	*ddt_os;
	uint64_t	ddt_stat_object;
	uint64_t	ddt_object[DDT_TYPES][DDT_CLASSES];

	/*
	 * Held dnode for each store object, cached so repeat ZAP ops skip
	 * the per-call object->dnode resolution.  Set in ddt_object_create/
	 * load, released in ddt_object_destroy/table_free; NULL means the
	 * object does not exist (the authoritative presence gate, see
	 * ddt_object_exists()).  Protected by ddt_objects_lock like
	 * ddt_object[].  Ported from OpenZFS ff5414406.
	 */
	dnode_t		*ddt_object_dnode[DDT_TYPES][DDT_CLASSES];
	ddt_histogram_t	ddt_histogram[DDT_TYPES][DDT_CLASSES];
	ddt_histogram_t	ddt_histogram_cache[DDT_TYPES][DDT_CLASSES];
	ddt_object_t	ddt_object_stats[DDT_TYPES][DDT_CLASSES];

	/* DDT log (journal) state; see ddt_log.c. */
	ddt_log_t	ddt_log[2];		/* active/flushing logs */
	ddt_log_t	*ddt_log_active;	/* points into ddt_log[] */
	ddt_log_t	*ddt_log_flushing;	/* swapped when flush starts */
	int32_t		ddt_log_ingest_rate;	/* rolling log ingest rate */
	int32_t		ddt_log_flush_rate;	/* rolling log flush rate */
	int32_t		ddt_log_flush_time_rate; /* avg time spent flushing */
	uint32_t	ddt_log_flush_pressure;	/* pressure to apply for cap */
	uint32_t	ddt_log_flush_prev_backlog; /* previous backlog size */
	uint64_t	ddt_flush_force_txg;	/* flush hard before this txg */
	ddt_object_t	ddt_log_stats;		/* logged-entry stats */
	ddt_histogram_t	ddt_log_histogram;	/* logged-entry histogram */

	avl_node_t	ddt_node;
};

/*
 * Buffer size for ddt_object_name() output.  Public because zdb formats
 * DDT object names through this same helper.
 */
#define	DDT_NAMELEN	80

extern void ddt_object_name(ddt_t *ddt, ddt_type_t type,
    ddt_class_t class, char *name);
extern int ddt_object_walk(ddt_t *ddt, ddt_type_t type,
    ddt_class_t class, uint64_t *walk, ddt_entry_t *dde);
extern int ddt_object_count(ddt_t *ddt, ddt_type_t type,
    ddt_class_t class, uint64_t *count);
extern int ddt_object_info(ddt_t *ddt, ddt_type_t type,
    ddt_class_t class, dmu_object_info_t *);
extern boolean_t ddt_object_exists(ddt_t *ddt, ddt_type_t type,
    ddt_class_t class);

extern void ddt_bp_fill(const ddt_univ_phys_t *ddp, ddt_phys_variant_t v,
    blkptr_t *bp, uint64_t txg);
extern void ddt_bp_create(enum zio_checksum checksum, const ddt_key_t *ddk,
    const ddt_univ_phys_t *ddp, ddt_phys_variant_t v, blkptr_t *bp);

extern void ddt_key_fill(ddt_key_t *ddk, const blkptr_t *bp);

extern void ddt_phys_fill(ddt_univ_phys_t *ddp, ddt_phys_variant_t v,
    const blkptr_t *bp);
extern void ddt_phys_clear(ddt_univ_phys_t *ddp, ddt_phys_variant_t v);
extern void ddt_phys_addref(ddt_univ_phys_t *ddp, ddt_phys_variant_t v);
extern void ddt_phys_decref(ddt_univ_phys_t *ddp, ddt_phys_variant_t v);
extern void ddt_phys_free(ddt_t *ddt, ddt_key_t *ddk, ddt_univ_phys_t *ddp,
    ddt_phys_variant_t v, uint64_t txg);
extern ddt_phys_variant_t ddt_phys_select(const ddt_t *ddt,
    const ddt_entry_t *dde, const blkptr_t *bp);
extern uint64_t ddt_phys_birth(const ddt_univ_phys_t *ddp,
    ddt_phys_variant_t v);
extern int ddt_phys_is_gang(const ddt_univ_phys_t *ddp,
    ddt_phys_variant_t v);
extern int ddt_phys_dva_count(const ddt_univ_phys_t *ddp,
    ddt_phys_variant_t v, boolean_t encrypted);
extern uint64_t ddt_phys_refcnt(const ddt_univ_phys_t *ddp,
    ddt_phys_variant_t v);
extern uint64_t ddt_phys_total_refcnt(const ddt_t *ddt,
    const ddt_univ_phys_t *ddp);

extern void ddt_stat_add(ddt_stat_t *dst, const ddt_stat_t *src, uint64_t neg);

extern void ddt_histogram_add(ddt_histogram_t *dst, const ddt_histogram_t *src);
extern void ddt_histogram_stat(ddt_stat_t *dds, const ddt_histogram_t *ddh);
extern boolean_t ddt_histogram_empty(const ddt_histogram_t *ddh);
extern void ddt_get_dedup_object_stats(spa_t *spa, ddt_object_t *ddo);
extern uint64_t ddt_get_ddt_dsize(spa_t *spa);
extern void ddt_get_dedup_histogram(spa_t *spa, ddt_histogram_t *ddh);
extern void ddt_get_dedup_stats(spa_t *spa, ddt_stat_t *dds_total);

extern uint64_t ddt_get_dedup_dspace(spa_t *spa);
extern uint64_t ddt_get_pool_dedup_ratio(spa_t *spa);

extern int ddt_ditto_copies_needed(ddt_t *ddt, ddt_entry_t *dde,
    ddt_univ_phys_t *ddp_willref);
extern int ddt_ditto_copies_present(ddt_entry_t *dde);

extern size_t ddt_compress(void *src, uchar_t *dst, size_t s_len, size_t d_len);
extern void ddt_decompress(uchar_t *src, void *dst, size_t s_len, size_t d_len);

extern ddt_t *ddt_select(spa_t *spa, const blkptr_t *bp);
extern void ddt_enter(ddt_t *ddt);
extern void ddt_exit(ddt_t *ddt);
extern ddt_entry_t *ddt_lookup(ddt_t *ddt, const blkptr_t *bp,
    boolean_t verify);
extern void ddt_prefetch(spa_t *spa, const blkptr_t *bp);
extern void ddt_prefetch_all(spa_t *spa);
extern void ddt_remove(ddt_t *ddt, ddt_entry_t *dde);

extern boolean_t ddt_class_contains(spa_t *spa, ddt_class_t max_class,
    const blkptr_t *bp);

extern ddt_entry_t *ddt_repair_start(ddt_t *ddt, const blkptr_t *bp);
extern void ddt_repair_done(ddt_t *ddt, ddt_entry_t *dde);

extern int ddt_key_compare(const void *x1, const void *x2);
extern int ddt_entry_compare(const void *x1, const void *x2);
extern void ddt_init(void);
extern void ddt_fini(void);

extern void ddt_create(spa_t *spa);
extern int ddt_load(spa_t *spa);
extern void ddt_unload(spa_t *spa);
extern void ddt_sync(spa_t *spa, uint64_t txg);
extern int ddt_walk(spa_t *spa, ddt_bookmark_t *ddb, ddt_entry_t *dde);


/* DDT pruning (zpool ddtprune); see ddt.c. */
extern int ddt_prune_unique_entries(spa_t *spa, zpool_ddt_prune_unit_t unit,
    uint64_t amount);
extern void ddt_prune_walk(spa_t *spa, uint64_t cutoff,
    ddt_age_histo_t *histogram);
extern int ddt_object_update(ddt_t *ddt, ddt_type_t type,
    ddt_class_t class, const ddt_lightweight_entry_t *ddlwe, dmu_tx_t *tx);

#ifdef	__cplusplus
}
#endif

/*
 * Read-side accessors that return NULL when dde_io hasn't been
 * allocated.  Safe to call at any time; the caller is responsible
 * for serialisation when interpreting the result.
 */
#define	DDE_LEAD_ZIO(dde, p)	\
	((dde)->dde_io != NULL ? (dde)->dde_io->dde_lead_zio[p] : NULL)
#define	DDE_REPAIR_ABD(dde)	\
	((dde)->dde_io != NULL ? (dde)->dde_io->dde_repair_abd : NULL)

/*
 * Lazy I/O-state allocator.  Idempotent; caller must hold ddt_lock.
 */
extern void ddt_io_alloc(ddt_entry_t *dde);
extern void ddt_io_free(ddt_entry_t *dde);

/* Copy / revert a single phys variant.  See ddt.c for semantics. */
extern void ddt_phys_copy(ddt_univ_phys_t *dst, const ddt_univ_phys_t *src,
    ddt_phys_variant_t v);
extern void ddt_phys_unextend(ddt_univ_phys_t *cur,
    const ddt_univ_phys_t *orig, ddt_phys_variant_t v);
extern void ddt_phys_extend(ddt_univ_phys_t *ddp, ddt_phys_variant_t v,
    const blkptr_t *bp);

#endif	/* _SYS_DDT_H */
