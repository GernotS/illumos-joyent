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
 * Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2016 by Delphix. All rights reserved.
 * Copyright (c) 2023, Klara Inc.
 * Copyright 2026 Gernot Strasser
 */

/*
 * DDT internals.  These declarations are shared between the DDT subsystem
 * source files (ddt.c, ddt_zap.c, and in later patches ddt_log.c and
 * ddt_stats.c) and must not be exposed to callers outside the dedup
 * subsystem.  External consumers should include <sys/ddt.h> instead.
 *
 * This split mirrors the OpenZFS layout, where it was introduced as a
 * prerequisite for the Fast Dedup (FDT) work in PR #15892.  It is pure
 * code motion: no symbol gains or loses linkage, and the on-disk format
 * is unchanged.
 */

#ifndef	_SYS_DDT_IMPL_H
#define	_SYS_DDT_IMPL_H

#include <sys/ddt.h>
#include <sys/avl.h>
#include <sys/bitops.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	DDT_TYPE_CURRENT		0

#define	DDT_COMPRESS_BYTEORDER_MASK	0x80
#define	DDT_COMPRESS_FUNCTION_MASK	0x7f

/* Names of well-known entries inside a per-DDT root ZAP (FDT only). */
#define	DDT_DIR_VERSION		"version"
#define	DDT_DIR_FLAGS		"flags"

/*
 * On-disk DDT log format.  See ddt_log.c.  These structures are part of the
 * on-disk layout and must stay byte-compatible with OpenZFS (PR #15895).
 */

#define	DDT_ENTRY_TO_LIGHTWEIGHT(ddt, dde, ddlwe) do {			\
	memset((ddlwe), 0, sizeof (*ddlwe));				\
	(ddlwe)->ddlwe_key = (dde)->dde_key;				\
	(ddlwe)->ddlwe_type = (dde)->dde_type;				\
	(ddlwe)->ddlwe_class = (dde)->dde_class;			\
	memcpy(&(ddlwe)->ddlwe_phys, (dde)->dde_phys, DDT_PHYS_SIZE(ddt)); \
} while (0)

#define	DDT_LOG_ENTRY_TO_LIGHTWEIGHT(ddt, ddle, ddlwe) do {		\
	memset((ddlwe), 0, sizeof (*ddlwe));				\
	(ddlwe)->ddlwe_key = (ddle)->ddle_key;				\
	(ddlwe)->ddlwe_type = (ddle)->ddle_type;			\
	(ddlwe)->ddlwe_class = (ddle)->ddle_class;			\
	memcpy(&(ddlwe)->ddlwe_phys, (ddle)->ddle_phys, DDT_PHYS_SIZE(ddt)); \
} while (0)

/*
 * An entry on the log tree.  These are "frozen", a record of what's in the
 * on-disk log.  They can't be used in place, but can be "loaded" back into
 * the live tree.
 */
typedef struct {
	ddt_key_t	ddle_key;	/* ddt_log_tree key */
	avl_node_t	ddle_node;	/* ddt_log_tree node */
	ddt_type_t	ddle_type;	/* storage type */
	ddt_class_t	ddle_class;	/* storage class */
	/* extra allocation for flat/trad phys */
	ddt_univ_phys_t	ddle_phys[];
} ddt_log_entry_t;

/* On-disk log record types. */
typedef enum {
	DLR_INVALID	= 0,	/* end of block marker */
	DLR_ENTRY	= 1,	/* an entry to add or replace in the log tree */
} ddt_log_record_type_t;

/* On-disk log record header. */
typedef struct {
	/*
	 * dlr_info is a packed u64, use the DLR_GET/DLR_SET macros below to
	 * access it.
	 *
	 * bits 0-7:    record type (ddt_log_record_type_t)
	 * bits 8-15:   length of record header+payload
	 * bits 16-47:  reserved, all zero
	 * bits 48-55:  if type==DLR_ENTRY, storage type (ddt_type)
	 *		  otherwise all zero
	 * bits 56-63:  if type==DLR_ENTRY, storage class (ddt_class)
	 *		  otherwise all zero
	 */
	uint64_t	dlr_info;
	uint8_t		dlr_payload[];
} ddt_log_record_t;

#define	DLR_GET_TYPE(dlr)		BF64_GET((dlr)->dlr_info, 0, 8)
#define	DLR_SET_TYPE(dlr, v)		BF64_SET((dlr)->dlr_info, 0, 8, v)
#define	DLR_GET_RECLEN(dlr)		BF64_GET((dlr)->dlr_info, 8, 16)
#define	DLR_SET_RECLEN(dlr, v)		BF64_SET((dlr)->dlr_info, 8, 16, v)
#define	DLR_GET_ENTRY_TYPE(dlr)		BF64_GET((dlr)->dlr_info, 48, 8)
#define	DLR_SET_ENTRY_TYPE(dlr, v)	BF64_SET((dlr)->dlr_info, 48, 8, v)
#define	DLR_GET_ENTRY_CLASS(dlr)	BF64_GET((dlr)->dlr_info, 56, 8)
#define	DLR_SET_ENTRY_CLASS(dlr, v)	BF64_SET((dlr)->dlr_info, 56, 8, v)

/* Payload for DLR_ENTRY. */
typedef struct {
	ddt_key_t	dlre_key;
	ddt_univ_phys_t	dlre_phys[];
} ddt_log_record_entry_t;

/* Log flags (ddl_flags, dlh_flags) */
#define	DDL_FLAG_FLUSHING	(1 << 0)	/* this log is being flushed */
#define	DDL_FLAG_CHECKPOINT	(1 << 1)	/* header has a checkpoint */

/* On-disk log header, stored in the bonus buffer. */
typedef struct {
	/*
	 * dlh_info is a packed u64, use the DLH_GET/DLH_SET macros below to
	 * access it.
	 *
	 * bits 0-7:   log version
	 * bits 8-15:  log flags
	 * bits 16-63: reserved, all zero
	 */
	uint64_t	dlh_info;
	uint64_t	dlh_length;	/* log size in bytes */
	uint64_t	dlh_first_txg;	/* txg this log went active */
	ddt_key_t	dlh_checkpoint;	/* last checkpoint */
} ddt_log_header_t;

#define	DLH_GET_VERSION(dlh)	BF64_GET((dlh)->dlh_info, 0, 8)
#define	DLH_SET_VERSION(dlh, v)	BF64_SET((dlh)->dlh_info, 0, 8, v)
#define	DLH_GET_FLAGS(dlh)	BF64_GET((dlh)->dlh_info, 8, 8)
#define	DLH_SET_FLAGS(dlh, v)	BF64_SET((dlh)->dlh_info, 8, 8, v)

/*
 * In-core state threaded through a single log-append operation
 * (ddt_log_begin/entry/commit).
 */
typedef struct {
	dmu_tx_t	*dlu_tx;		/* tx being applied to */
	dnode_t		*dlu_dn;	/* log object dnode */
	dmu_buf_t	**dlu_dbp;	/* array of block buffer pointers */
	int		dlu_ndbp;	/* number of block buffer pointers */
	uint16_t	dlu_reclen;	/* cached length of record */
	uint64_t	dlu_block;	/* block for next entry */
	uint64_t	dlu_offset;	/* offset for next entry */
} ddt_log_update_t;

/* Sentinel ddt_version value, before ddt_configure() runs. */
#define	DDT_VERSION_UNCONFIGURED	(UINT64_MAX)

/*
 * Default flags for each DDT version.  Indexed by ddt_version.  Currently
 * both versions imply zero flags (FLAT/LOG default-off); flags become
 * meaningful in later Fast Dedup patches.
 */
extern const uint64_t ddt_version_flags[];

extern int ddt_configure(ddt_t *ddt, boolean_t new);
extern void ddt_create_dir(ddt_t *ddt, dmu_tx_t *tx);
extern void ddt_destroy_dir(ddt_t *ddt, dmu_tx_t *tx);

/*
 * Ops vector to access a specific DDT object type.
 */
typedef struct ddt_ops {
	char ddt_op_name[32];
	int (*ddt_op_create)(objset_t *os, uint64_t *object, dmu_tx_t *tx,
	    boolean_t prehash);
	int (*ddt_op_destroy)(objset_t *os, uint64_t object, dmu_tx_t *tx);
	int (*ddt_op_lookup)(dnode_t *dn,
	    const ddt_key_t *ddk, void *phys, size_t psize);
	int (*ddt_op_contains)(dnode_t *dn,
	    const ddt_key_t *ddk);
	void (*ddt_op_prefetch)(dnode_t *dn,
	    ddt_entry_t *dde);
	void (*ddt_op_prefetch_all)(dnode_t *dn);
	int (*ddt_op_update)(dnode_t *dn,
	    const ddt_key_t *ddk, const void *phys, size_t psize,
	    dmu_tx_t *tx);
	int (*ddt_op_remove)(dnode_t *dn,
	    const ddt_key_t *ddk, dmu_tx_t *tx);
	int (*ddt_op_walk)(dnode_t *dn, ddt_entry_t *dde,
	    uint64_t *walk);
	int (*ddt_op_count)(dnode_t *dn, uint64_t *count);
} ddt_ops_t;

extern const ddt_ops_t ddt_zap_ops;

/* ddt_stats.c — non-static helpers used by ddt.c sync/load paths */
extern void ddt_stat_generate(ddt_t *ddt, ddt_entry_t *dde, ddt_stat_t *dds);
extern void ddt_stat_update(ddt_t *ddt, ddt_entry_t *dde, uint64_t neg);
extern void ddt_histogram_add_entry(ddt_t *ddt, ddt_histogram_t *ddh,
    const ddt_lightweight_entry_t *ddlwe);
extern void ddt_histogram_sub_entry(ddt_t *ddt, ddt_histogram_t *ddh,
    const ddt_lightweight_entry_t *ddlwe);

/* ddt_log.c — dedup log (PR #15895) */
extern void ddt_log_update_stats(ddt_t *ddt);
extern void ddt_log_begin(ddt_t *ddt, size_t nentries, dmu_tx_t *tx,
    ddt_log_update_t *dlu);
extern void ddt_log_entry(ddt_t *ddt, ddt_lightweight_entry_t *ddlwe,
    ddt_log_update_t *dlu);
extern void ddt_log_commit(ddt_t *ddt, ddt_log_update_t *dlu);
extern boolean_t ddt_log_take_first(ddt_t *ddt, ddt_log_t *ddl,
    ddt_lightweight_entry_t *ddlwe);
extern boolean_t ddt_log_take_key(ddt_t *ddt, ddt_log_t *ddl,
    const ddt_key_t *ddk, ddt_lightweight_entry_t *ddlwe);
extern boolean_t ddt_log_find_key(ddt_t *ddt, const ddt_key_t *ddk,
    ddt_lightweight_entry_t *ddlwe, boolean_t *from_flushing);
extern boolean_t ddt_log_remove_key(ddt_t *ddt, ddt_log_t *ddl,
    const ddt_key_t *ddk);
extern void ddt_log_checkpoint(ddt_t *ddt, ddt_lightweight_entry_t *ddlwe,
    dmu_tx_t *tx);
extern void ddt_log_truncate(ddt_t *ddt, dmu_tx_t *tx);
extern boolean_t ddt_log_swap(ddt_t *ddt, dmu_tx_t *tx);
extern void ddt_log_destroy(ddt_t *ddt, dmu_tx_t *tx);
extern int ddt_log_load(ddt_t *ddt);
extern void ddt_log_alloc(ddt_t *ddt);
extern void ddt_log_free(ddt_t *ddt);
extern void ddt_log_init(void);
extern void ddt_log_fini(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_DDT_IMPL_H */
