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
 * Copyright (c) 2023, Klara Inc.
 * Copyright 2026 Gernot Strasser
 */

/*
 * DDT statistics and histogram bookkeeping.  Split out of ddt.c with no
 * semantic change, ahead of the OpenZFS Fast Dedup port.  The two static
 * helpers (ddt_stat_generate, ddt_stat_update) lose their static linkage
 * because ddt.c still calls them from the sync/load paths; their extern
 * declarations live in <sys/ddt_impl.h>.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/ddt.h>
#include <sys/ddt_impl.h>

void
ddt_stat_generate(ddt_t *ddt, ddt_entry_t *dde, ddt_stat_t *dds)
{
	spa_t *spa = ddt->ddt_spa;
	ddt_univ_phys_t *ddp = dde->dde_phys;
	ddt_key_t *ddk = &dde->dde_key;
	uint64_t lsize = DDK_GET_LSIZE(ddk);
	uint64_t psize = DDK_GET_PSIZE(ddk);

	bzero(dds, sizeof (*dds));

	int nphys = DDT_NPHYS(ddt);
	for (int p = 0; p < nphys; p++) {
		ddt_phys_t *slot = (ddt->ddt_flags & DDT_FLAG_FLAT) ?
		    (ddt_phys_t *)&ddp->ddp_flat : &ddp->ddp_trad[p];
		uint64_t dsize = 0;
		uint64_t refcnt = slot->ddp_refcnt;

		if (slot->ddp_phys_birth == 0)
			continue;

		for (int d = 0; d < DDE_GET_NDVAS(dde); d++)
			dsize += dva_get_dsize_sync(spa, &slot->ddp_dva[d]);

		dds->dds_blocks += 1;
		dds->dds_lsize += lsize;
		dds->dds_psize += psize;
		dds->dds_dsize += dsize;

		dds->dds_ref_blocks += refcnt;
		dds->dds_ref_lsize += lsize * refcnt;
		dds->dds_ref_psize += psize * refcnt;
		dds->dds_ref_dsize += dsize * refcnt;
	}
}

void
ddt_stat_add(ddt_stat_t *dst, const ddt_stat_t *src, uint64_t neg)
{
	const uint64_t *s = (const uint64_t *)src;
	uint64_t *d = (uint64_t *)dst;
	uint64_t *d_end = (uint64_t *)(dst + 1);

	ASSERT(neg == 0 || neg == -1ULL);	/* add or subtract */

	while (d < d_end)
		*d++ += (*s++ ^ neg) - neg;
}

void
ddt_stat_update(ddt_t *ddt, ddt_entry_t *dde, uint64_t neg)
{
	ddt_stat_t dds;
	ddt_histogram_t *ddh;
	int bucket;

	ddt_stat_generate(ddt, dde, &dds);

	bucket = highbit64(dds.dds_ref_blocks) - 1;
	ASSERT(bucket >= 0);

	ddh = &ddt->ddt_histogram[dde->dde_type][dde->dde_class];

	ddt_stat_add(&ddh->ddh_stat[bucket], &dds, neg);
}

void
ddt_histogram_add(ddt_histogram_t *dst, const ddt_histogram_t *src)
{
	for (int h = 0; h < 64; h++)
		ddt_stat_add(&dst->ddh_stat[h], &src->ddh_stat[h], 0);
}

/*
 * Lightweight-entry variant of ddt_stat_generate(), reading from a stack
 * ddt_lightweight_entry_t instead of a live ddt_entry_t.  Used by the DDT log
 * (ddt_log.c), which deals in lightweight entries.  Equivalent to OpenZFS's
 * ddlwe-based ddt_stat_generate() (PR #15895); kept as a separate helper so
 * the existing dde-based ddt_stat_generate() and its callers are undisturbed.
 */
static void
ddt_stat_generate_lightweight(ddt_t *ddt,
    const ddt_lightweight_entry_t *ddlwe, ddt_stat_t *dds)
{
	spa_t *spa = ddt->ddt_spa;
	const ddt_univ_phys_t *ddp = &ddlwe->ddlwe_phys;
	const ddt_key_t *ddk = &ddlwe->ddlwe_key;
	uint64_t lsize = DDK_GET_LSIZE(ddk);
	uint64_t psize = DDK_GET_PSIZE(ddk);
	int ndvas = DDK_GET_CRYPT(ddk) ? SPA_DVAS_PER_BP - 1 : SPA_DVAS_PER_BP;

	bzero(dds, sizeof (*dds));

	int nphys = DDT_NPHYS(ddt);
	for (int p = 0; p < nphys; p++) {
		const ddt_phys_t *slot = (ddt->ddt_flags & DDT_FLAG_FLAT) ?
		    (const ddt_phys_t *)&ddp->ddp_flat : &ddp->ddp_trad[p];
		uint64_t dsize = 0;
		uint64_t refcnt = slot->ddp_refcnt;

		if (slot->ddp_phys_birth == 0)
			continue;

		for (int d = 0; d < ndvas; d++)
			dsize += dva_get_dsize_sync(spa, &slot->ddp_dva[d]);

		dds->dds_blocks += 1;
		dds->dds_lsize += lsize;
		dds->dds_psize += psize;
		dds->dds_dsize += dsize;

		dds->dds_ref_blocks += refcnt;
		dds->dds_ref_lsize += lsize * refcnt;
		dds->dds_ref_psize += psize * refcnt;
		dds->dds_ref_dsize += dsize * refcnt;
	}
}

void
ddt_histogram_add_entry(ddt_t *ddt, ddt_histogram_t *ddh,
    const ddt_lightweight_entry_t *ddlwe)
{
	ddt_stat_t dds;
	int bucket;

	ddt_stat_generate_lightweight(ddt, ddlwe, &dds);

	bucket = highbit64(dds.dds_ref_blocks) - 1;
	if (bucket < 0)
		return;

	ddt_stat_add(&ddh->ddh_stat[bucket], &dds, 0);
}

void
ddt_histogram_sub_entry(ddt_t *ddt, ddt_histogram_t *ddh,
    const ddt_lightweight_entry_t *ddlwe)
{
	ddt_stat_t dds;
	int bucket;

	ddt_stat_generate_lightweight(ddt, ddlwe, &dds);

	bucket = highbit64(dds.dds_ref_blocks) - 1;
	if (bucket < 0)
		return;

	ddt_stat_add(&ddh->ddh_stat[bucket], &dds, -1ULL);
}

void
ddt_histogram_stat(ddt_stat_t *dds, const ddt_histogram_t *ddh)
{
	bzero(dds, sizeof (*dds));

	for (int h = 0; h < 64; h++)
		ddt_stat_add(dds, &ddh->ddh_stat[h], 0);
}

boolean_t
ddt_histogram_empty(const ddt_histogram_t *ddh)
{
	const uint64_t *s = (const uint64_t *)ddh;
	const uint64_t *s_end = (const uint64_t *)(ddh + 1);

	while (s < s_end)
		if (*s++ != 0)
			return (B_FALSE);

	return (B_TRUE);
}

void
ddt_get_dedup_object_stats(spa_t *spa, ddt_object_t *ddo_total)
{
	/*
	 * The totals are accumulated below, so start from zero rather than
	 * trusting every caller to pass a zeroed struct; ddt_total_entries()
	 * passed an uninitialized one.  Matches upstream OpenZFS.
	 */
	bzero(ddo_total, sizeof (*ddo_total));

	/* Sum the statistics we cached in ddt_object_sync(). */
	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		ddt_t *ddt = spa->spa_ddt[c];
		for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
			for (ddt_class_t class = 0; class < DDT_CLASSES;
			    class++) {
				ddt_object_t *ddo =
				    &ddt->ddt_object_stats[type][class];
				ddo_total->ddo_count += ddo->ddo_count;
				ddo_total->ddo_dspace += ddo->ddo_dspace;
				ddo_total->ddo_mspace += ddo->ddo_mspace;
			}
		}

		/* Include logged-but-not-yet-flushed entries (dedup log). */
		ddt_object_t *ddo = &ddt->ddt_log_stats;
		ddo_total->ddo_count += ddo->ddo_count;
		ddo_total->ddo_dspace += ddo->ddo_dspace;
		ddo_total->ddo_mspace += ddo->ddo_mspace;
	}

	/* ... and compute the averages. */
	if (ddo_total->ddo_count != 0) {
		ddo_total->ddo_dspace /= ddo_total->ddo_count;
		ddo_total->ddo_mspace /= ddo_total->ddo_count;
	}
}

/*
 * Return the total on-disk size of all DDTs on the pool (raw, not averaged),
 * summed from the per-object stats cached during ddt_object_sync().  Backs the
 * dedup_table_size pool property and dedup-quota accounting.  Equivalent to
 * OpenZFS's ddt_get_ddt_dsize() (PR #15889) but reuses illumos's existing
 * ddt_object_stats[][] cache instead of recomputing, so the averaging
 * ddt_get_dedup_object_stats() consumers are left untouched.
 */
uint64_t
ddt_get_ddt_dsize(spa_t *spa)
{
	dmu_object_info_t doi;
	uint64_t dsize = 0;

	if (spa->spa_dedup_dsize != ~0ULL)
		return (spa->spa_dedup_dsize);

	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		ddt_t *ddt = spa->spa_ddt[c];
		if (ddt == NULL)
			continue;

		/*
		 * Live-sample the store objects.  The sync-context cache
		 * (ddt_object_stats[][]) is refreshed in the same txg that
		 * dirties the objects -- before the dnode sync applies the
		 * writes -- so it is one txg behind, and permanently so once
		 * the table goes idle (e.g. after a flush-only txg).  We are
		 * in open context: dmu_object_info() reports the last-synced
		 * state.  ddt_objects_lock keeps sync-context object
		 * destruction away (see ddt_object_destroy()).
		 */
		rw_enter(&ddt->ddt_objects_lock, RW_READER);
		for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
			for (ddt_class_t class = 0; class < DDT_CLASSES;
			    class++) {
				if (ddt->ddt_object[type][class] == 0)
					continue;
				if (dmu_object_info(ddt->ddt_os,
				    ddt->ddt_object[type][class], &doi) != 0)
					continue;
				dsize += doi.doi_physical_blocks_512 << 9;
			}
		}
		rw_exit(&ddt->ddt_objects_lock);

		/*
		 * Same for the two log objects.  Sample them directly rather
		 * than through ddt_log_update_stats(), which rewrites
		 * ddt_log_stats and reads the log trees that sync context
		 * changes, without any lock.  Their object numbers don't
		 * change while the log exists, and swapping only exchanges
		 * which one is active.
		 */
		if (ddt->ddt_flags & DDT_FLAG_LOG) {
			for (int n = 0; n < 2; n++) {
				uint64_t obj = ddt->ddt_log[n].ddl_object;
				if (obj == 0 || dmu_object_info(ddt->ddt_os,
				    obj, &doi) != 0)
					continue;
				dsize += doi.doi_physical_blocks_512 << 9;
			}
		}
	}

	spa->spa_dedup_dsize = dsize;
	return (dsize);
}

void
ddt_get_dedup_histogram(spa_t *spa, ddt_histogram_t *ddh)
{
	for (enum zio_checksum c = 0; c < ZIO_CHECKSUM_FUNCTIONS; c++) {
		ddt_t *ddt = spa->spa_ddt[c];
		for (ddt_type_t type = 0; type < DDT_TYPES; type++) {
			for (ddt_class_t class = 0; class < DDT_CLASSES;
			    class++) {
				ddt_histogram_add(ddh,
				    &ddt->ddt_histogram_cache[type][class]);
			}
		}

		/* Logged-but-not-yet-flushed entries (dedup log). */
		ddt_histogram_add(ddh, &ddt->ddt_log_histogram);
	}
}

void
ddt_get_dedup_stats(spa_t *spa, ddt_stat_t *dds_total)
{
	ddt_histogram_t *ddh_total;

	ddh_total = kmem_zalloc(sizeof (ddt_histogram_t), KM_SLEEP);
	ddt_get_dedup_histogram(spa, ddh_total);
	ddt_histogram_stat(dds_total, ddh_total);
	kmem_free(ddh_total, sizeof (ddt_histogram_t));
}

uint64_t
ddt_get_dedup_dspace(spa_t *spa)
{
	ddt_stat_t dds_total = { 0 };

	ddt_get_dedup_stats(spa, &dds_total);
	return (dds_total.dds_ref_dsize - dds_total.dds_dsize);
}

uint64_t
ddt_get_pool_dedup_ratio(spa_t *spa)
{
	ddt_stat_t dds_total = { 0 };

	ddt_get_dedup_stats(spa, &dds_total);
	if (dds_total.dds_dsize == 0)
		return (100);

	return (dds_total.dds_ref_dsize * 100 / dds_total.dds_dsize);
}
