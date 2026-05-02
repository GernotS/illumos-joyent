/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Eduard Siboshvili
 */

/*
 * RX/TX ring model
 * ----------------
 *
 * This file implements the packet datapath for the re(4D) driver,
 * including RX/TX descriptor rings, buffer management, submission,
 * completion and drain during stop/reset.
 *
 *
 * Descriptor formats
 * ------------------
 *
 * Supported Realtek devices use multiple RX descriptor layouts depending
 * on MAC revision (notably descriptor types V3 and V4).  These formats are
 * not layout-compatible.
 *
 * Descriptor field access (opts1, opts2, DMA address) must not rely on a
 * single C structure.  All accesses are performed through helper routines
 * to isolate format differences.
 *
 *
 * RX ring model
 * -------------
 *
 * RX descriptors are owned either by hardware or the driver:
 *
 *   OWN set   -> descriptor is owned by hardware
 *   OWN clear -> descriptor is owned by driver
 *
 * RX processing advances from rx_cons until a descriptor still owned by
 * hardware is encountered or the processing budget is reached.
 *
 * Packets may span multiple descriptors.  The driver assembles packet
 * chains using first/last segment indicators before passing them to MAC.
 *
 * Refill must install a replacement buffer and fully initialize the
 * descriptor before returning ownership to hardware.  The OWN bit is the
 * ownership transfer point and must be set only after all descriptor
 * fields are valid.
 *
 *
 * TX ring model
 * -------------
 *
 * TX uses producer/consumer indexing:
 *
 *   tx_prod -> next descriptor to fill
 *   tx_cons -> next descriptor to reclaim
 *
 * TX submission maps packet data for DMA, fills descriptors and publishes
 * them to hardware.  TX reclaim releases completed descriptors and frees
 * associated resources.
 *
 *
 * Checksum and offload
 * --------------------
 *
 * Hardware checksum offload is used where supported.  Behavior may vary
 * across revisions; the datapath selects appropriate handling based on
 * device configuration.
 *
 *
 * Ring drain and stop/reset
 * -------------------------
 *
 * Stop/reset must prevent new RX/TX activity and interrupt scheduling
 * before draining rings.
 *
 * RX/TX drain may wait for hardware completion and taskq activity.
 * Waiting for interrupt tasks must be performed outside the main driver
 * mutex to avoid deadlocks, as interrupt handlers may require the same
 * locks.
 *
 * Drain operations must be bounded.  If hardware does not return ownership
 * within a reasonable time, reset should proceed rather than blocking
 * indefinitely.
*/


#include "re.h"

#define RE_TX_DESC_RING_BYTES(n_desc) ((size_t)(n_desc) * sizeof (struct TxDesc))
#define RE_RX_DESC_RING_BYTES_TYPE_1(n_desc) ((size_t)(n_desc) * sizeof (struct RxDesc))
#define RE_RX_DESC_RING_BYTES_TYPE_3(n_desc) ((size_t)(n_desc) * sizeof (struct RxDescV3))
#define RE_RX_DESC_RING_BYTES_TYPE_4(n_desc) ((size_t)(n_desc) * sizeof (struct RxDescV4))

#define RE_MIN_PATCH_LEN 47
#define RE_PATCH_PKT_MAX 175

typedef struct re_tx_plan {
	uint32_t cksum_flags;
	uint32_t lso_flags;

	uint32_t opts1_0;
	uint32_t opts2_0;

	uint32_t mss;
	uint32_t l4_offset;
	uint32_t min_pkt_len;

	boolean_t is_ipv4;
	boolean_t is_ipv6;
	boolean_t is_tcp;
	boolean_t is_udp;

	boolean_t use_hw_csum;
	boolean_t use_lso;
	boolean_t pad_applied;
} re_tx_plan_t;

static const ddi_device_acc_attr_t re_desc_accattr = {
    DDI_DEVICE_ATTR_V1,
    DDI_NEVERSWAP_ACC,
    DDI_STRICTORDER_ACC,
    DDI_DEFAULT_ACC
};

static const ddi_dma_attr_t re_tx_desc_dma_attr = {
    .dma_attr_version    = DMA_ATTR_V0,
    .dma_attr_addr_lo    = 0,
    .dma_attr_addr_hi    = UINT64_MAX,
    .dma_attr_count_max  = UINT64_MAX,
    .dma_attr_align      = RE_ALIGN,
    .dma_attr_burstsizes = 0xFFFFFFFF,
    .dma_attr_minxfer    = 1,
    .dma_attr_maxxfer    = UINT64_MAX,
    .dma_attr_seg        = UINT64_MAX,
    .dma_attr_sgllen     = 1,
    .dma_attr_granular   = 1,
    .dma_attr_flags      = 0
};

static const ddi_dma_attr_t re_rx_desc_dma_attr = {
    .dma_attr_version    = DMA_ATTR_V0,
    .dma_attr_addr_lo    = 0,
    .dma_attr_addr_hi    = UINT64_MAX,
    .dma_attr_count_max  = UINT64_MAX,
    .dma_attr_align      = RE_ALIGN,
    .dma_attr_burstsizes = 0xFFFFFFFF,
    .dma_attr_minxfer    = 1,
    .dma_attr_maxxfer    = UINT64_MAX,
    .dma_attr_seg        = UINT64_MAX,
    .dma_attr_sgllen     = 1,
    .dma_attr_granular   = 1,
    .dma_attr_flags      = 0
};

static const ddi_device_acc_attr_t re_buf_accattr = {
    DDI_DEVICE_ATTR_V1,
    DDI_NEVERSWAP_ACC,
    DDI_STRICTORDER_ACC,
    DDI_DEFAULT_ACC
};

static const ddi_dma_attr_t re_tx_buf_dma_attr = {
    .dma_attr_version    = DMA_ATTR_V0,
    .dma_attr_addr_lo    = 0,
    .dma_attr_addr_hi    = UINT64_MAX,
    .dma_attr_count_max  = UINT64_MAX,
    .dma_attr_align      = 1,
    .dma_attr_burstsizes = 0xFFFFFFFF,
    .dma_attr_minxfer    = 1,
    .dma_attr_maxxfer    = UINT64_MAX,
    .dma_attr_seg        = UINT64_MAX,
    .dma_attr_sgllen     = RE_TX_NSEGS,
    .dma_attr_granular   = 1,
    .dma_attr_flags      = 0
};

static const ddi_dma_attr_t re_tx_copy_dma_attr = {
    .dma_attr_version    = DMA_ATTR_V0,
    .dma_attr_addr_lo    = 0,
    .dma_attr_addr_hi    = UINT64_MAX,
    .dma_attr_count_max  = UINT64_MAX,
    .dma_attr_align      = 1,
    .dma_attr_burstsizes = 0xFFFFFFFF,
    .dma_attr_minxfer    = 1,
    .dma_attr_maxxfer    = UINT64_MAX,
    .dma_attr_seg        = UINT64_MAX,
    .dma_attr_sgllen     = 1,
    .dma_attr_granular   = 1,
    .dma_attr_flags      = 0
};

static const ddi_dma_attr_t re_rx_buf_dma_attr = {
    .dma_attr_version    = DMA_ATTR_V0,
    .dma_attr_addr_lo    = 0,
    .dma_attr_addr_hi    = UINT64_MAX,
    .dma_attr_count_max  = UINT64_MAX,
    .dma_attr_align      = RE_ALIGN,
    .dma_attr_burstsizes = 0xFFFFFFFF,
    .dma_attr_minxfer    = 1,
    .dma_attr_maxxfer    = UINT64_MAX,
    .dma_attr_seg        = UINT64_MAX,
    .dma_attr_sgllen     = 1,
    .dma_attr_granular   = 1,
    .dma_attr_flags      = 0
};

static void re_rx_recycle(caddr_t arg);
static void re_tx_buf_recycle(re_tx_buffer_t *tb);
static int re_alloc_rings_desc(struct re_softc *sc);
static void re_free_rx_bufs(struct re_softc *sc);
static void re_free_tx_rings_bufs(struct re_softc *sc);
static int re_rx_buf_create_mblk(re_rx_buffer_t *rb);
static void re_rx_desc_rearm(struct re_softc *sc, re_rx_ring_t *rr, uint32_t idx, re_rx_buffer_t *rb);
static void re_rx_desc_sync(re_rx_ring_t *rr, uint32_t idx, uint_t sync_flag);
static void re_tx_ring_init_masks(re_tx_ring_t *tr);
static void re_free_tx_bufs(re_tx_ring_t *tr);
static void re_rx_assembly_reset(re_rx_ring_t *rr);



static int
re_get_rx_ring_size(struct re_softc *sc)
{
	int rx_desc_num = sc->rx_rings[0].num_desc;
	switch (sc->re_rx_ring_desc_type) {
		case RE_RX_RING_DESC_TYPE_4:
			return RE_RX_DESC_RING_BYTES_TYPE_4(rx_desc_num);
		case RE_RX_RING_DESC_TYPE_3:
			return RE_RX_DESC_RING_BYTES_TYPE_3(rx_desc_num);
		default:
			return RE_RX_DESC_RING_BYTES_TYPE_1(rx_desc_num);
	}
}

static size_t
re_get_rx_desc_size(struct re_softc *sc)
{
	switch (sc->re_rx_ring_desc_type) {
		case RE_RX_RING_DESC_TYPE_4:
			return sizeof (struct RxDescV4);
		case RE_RX_RING_DESC_TYPE_3:
			return sizeof (struct RxDescV3);
		default:
			return sizeof (struct RxDesc);
	}
}

static int
re_get_tx_ring_size(struct re_softc *sc)
{
	return RE_TX_DESC_RING_BYTES(sc->tx_rings[0].num_desc);
}

int
re_alloc_rings(struct re_softc *sc)
{
	int i;

	sc->rx_rings = kmem_zalloc(sc->num_rx_rings *
	    sizeof (re_rx_ring_t), KM_SLEEP);
	sc->tx_rings = kmem_zalloc(sc->num_tx_rings *
	    sizeof (re_tx_ring_t), KM_SLEEP);

	for (i = 0; i < sc->num_rx_rings; i++) {
		re_rx_ring_t *rr = &sc->rx_rings[i];

		rr->sc = sc;
		rr->index = i;
		rr->num_desc = RE_RX_LIST_CNT;
		rr->num_bufs = rr->num_desc * 2;
		rr->rrx_assem_head = NULL;
		rr->rrx_assem_tail = NULL;
		rr->rrx_assem_bytes = 0;
		rr->rrx_ip_csum_valid = B_FALSE;
		rr->rrx_ip_csum_ok = B_FALSE;
		rr->rrx_l4_csum_valid = B_FALSE;
		rr->rrx_l4_csum_ok = B_FALSE;
		mutex_init(&rr->rx_lock, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(sc->intr_pri));
		mutex_init(&rr->rx_free_lock, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(sc->intr_pri));
		rr->rx_lock_init = B_TRUE;

		cv_init(&rr->rx_free_cv, NULL, CV_DRIVER, NULL);
		rr->rx_free_cv_init = B_TRUE;

		if (!re_rx_ring_kstats_init(sc, rr))
			goto fail;
		rr->rx_kstat_init = B_TRUE;
	}

	for (i = 0; i < sc->num_tx_rings; i++) {
		re_tx_ring_t *tr = &sc->tx_rings[i];

		tr->sc = sc;
		tr->index = i;
		tr->num_desc = RE_TX_LIST_CNT;
		tr->tx_free_list_init = B_FALSE;

		mutex_init(&tr->tx_lock, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(sc->intr_pri));
		tr->tx_lock_init = B_TRUE;

		if (!re_tx_ring_kstat_init(sc, tr))
			goto fail;
		tr->tx_kstat_init = B_TRUE;
	}

	re_setup_rings_regs(sc);

	if (re_alloc_rings_desc(sc) != DDI_SUCCESS)
		goto fail;

	return (DDI_SUCCESS);

fail:
	re_free_rings(sc);
	return (DDI_FAILURE);
}

void
re_dma_block_free(re_dma_buffer_t *db)
{
	if (db == NULL)
		return;

	if (db->re_dma_hdl != NULL && db->ncookies != 0) {
		(void) ddi_dma_unbind_handle(db->re_dma_hdl);
		db->ncookies = 0;
		bzero(&db->cookie, sizeof (db->cookie));
	}

	if (db->re_acc_hdl != NULL) {
		ddi_dma_mem_free(&db->re_acc_hdl);
		db->re_acc_hdl = NULL;
		db->re_db_va = NULL;
		db->re_db_alloc_len = 0;
	}

	if (db->re_dma_hdl != NULL) {
		ddi_dma_free_handle(&db->re_dma_hdl);
		db->re_dma_hdl = NULL;
	}

	bzero(&db->cookie, sizeof (db->cookie));
}

void
re_free_rings_desc(struct re_softc *sc)
{
	int i;

	if (sc == NULL)
		return;

	if (sc->rx_rings != NULL) {
		for (i = 0; i < sc->num_rx_rings; i++) {
			re_rx_ring_t *rr = &sc->rx_rings[i];

			if (rr->rx_dma_desc.re_dma_hdl != NULL)
				re_dma_block_free(&rr->rx_dma_desc);

			rr->rx_desc = NULL;
			rr->rx_desc_stride = 0;
		}
	}

	if (sc->tx_rings != NULL) {
		for (i = 0; i < sc->num_tx_rings; i++) {
			re_tx_ring_t *tr = &sc->tx_rings[i];

			if (tr->tx_dma_desc.re_dma_hdl != NULL)
				re_dma_block_free(&tr->tx_dma_desc);

			tr->tx_desc = NULL;
		}
	}
}

void
re_free_rings(struct re_softc *sc)
{
	int i;

	if (sc == NULL)
		return;

	re_free_rx_bufs(sc);
	re_free_tx_rings_bufs(sc);
	re_free_rings_desc(sc);

	if (sc->rx_rings != NULL) {
		for (i = 0; i < sc->num_rx_rings; i++) {
			re_rx_ring_t *rr = &sc->rx_rings[i];

			if (rr->rx_kstat_init) {
				re_rx_ring_stats_fini(sc, rr);
				rr->rx_kstat_init = B_FALSE;
			}

			if (rr->rx_free_cv_init) {
				cv_destroy(&rr->rx_free_cv);
				rr->rx_free_cv_init = B_FALSE;
			}

			if (rr->rx_lock_init) {
				mutex_destroy(&rr->rx_lock);
				mutex_destroy(&rr->rx_free_lock);
				rr->rx_lock_init = B_FALSE;
			}
		}

		kmem_free(sc->rx_rings,
		    sizeof (re_rx_ring_t) * sc->num_rx_rings);
		sc->rx_rings = NULL;
	}

	if (sc->tx_rings != NULL) {
		for (i = 0; i < sc->num_tx_rings; i++) {
			re_tx_ring_t *tr = &sc->tx_rings[i];

			if (tr->tx_kstat_init) {
				re_tx_ring_kstat_fini(sc, tr);
				tr->tx_kstat_init = B_FALSE;
			}

			if (tr->tx_lock_init) {
				mutex_destroy(&tr->tx_lock);
				tr->tx_lock_init = B_FALSE;
			}
		}

		kmem_free(sc->tx_rings,
		    sizeof (re_tx_ring_t) * sc->num_tx_rings);
		sc->tx_rings = NULL;
	}
}

static int
re_dma_block_alloc(dev_info_t *dip, re_dma_buffer_t *db,
    const ddi_dma_attr_t *dma_attr, const ddi_device_acc_attr_t *acc_attr,
    size_t bytes, int bind_flags, int mem_flags)
{
	int err;
	uint_t ccount = 0;

	bzero(db, sizeof (*db));


	err = ddi_dma_alloc_handle(dip, (ddi_dma_attr_t *)dma_attr,
	    DDI_DMA_SLEEP, NULL, &db->re_dma_hdl);
	if (err != DDI_SUCCESS) {
		return (DDI_FAILURE);
	}

	err = ddi_dma_mem_alloc(db->re_dma_hdl, bytes,
	    (ddi_device_acc_attr_t *)acc_attr,
	    mem_flags, DDI_DMA_SLEEP, NULL,
	    &db->re_db_va, &db->re_db_alloc_len, &db->re_acc_hdl);
	if (err != DDI_SUCCESS) {
		ddi_dma_free_handle(&db->re_dma_hdl);
		db->re_dma_hdl = NULL;
		return (DDI_FAILURE);
	}

	err = ddi_dma_addr_bind_handle(db->re_dma_hdl, NULL,
	    db->re_db_va, db->re_db_alloc_len,
	    bind_flags, DDI_DMA_SLEEP, NULL,
	    &db->cookie, &ccount);

	db->ncookies = (uint32_t)ccount;

	if (err != DDI_DMA_MAPPED || ccount != 1) {

		if (err == DDI_DMA_MAPPED)
			(void) ddi_dma_unbind_handle(db->re_dma_hdl);

		ddi_dma_mem_free(&db->re_acc_hdl);
		db->re_acc_hdl = NULL;
		db->re_db_va = NULL;

		ddi_dma_free_handle(&db->re_dma_hdl);
		db->re_dma_hdl = NULL;
		db->ncookies = 0;
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

static int
re_alloc_tx_ring_desc(struct re_softc *sc, re_dma_buffer_t *ring_db)
{
    size_t bytes = re_get_tx_ring_size(sc);

    return re_dma_block_alloc(sc->dev, ring_db,
        &re_tx_desc_dma_attr, &re_desc_accattr,
        bytes,
        DDI_DMA_RDWR | DDI_DMA_CONSISTENT,
        DDI_DMA_CONSISTENT
    );
}

static int
re_alloc_rx_ring_desc(struct re_softc *sc, re_dma_buffer_t *ring_db)
{
    size_t bytes = re_get_rx_ring_size(sc);
    return re_dma_block_alloc(sc->dev, ring_db,
        &re_rx_desc_dma_attr, &re_desc_accattr,
        bytes,
        DDI_DMA_RDWR | DDI_DMA_CONSISTENT,
        DDI_DMA_CONSISTENT
    );
}

/*
static void
re_free_ring_desc(re_dma_buffer_t *ring_db)
{
    re_dma_block_free(ring_db);
}
*/

static int
re_alloc_rings_desc(struct re_softc *sc)
{
	int err;
	int rx_done = 0;
	int tx_done = 0;
	size_t desclen = re_get_rx_desc_size(sc);

	for (rx_done = 0; rx_done < sc->num_rx_rings; rx_done++) {
		re_rx_ring_t *rr = &sc->rx_rings[rx_done];

		err = re_alloc_rx_ring_desc(sc, &rr->rx_dma_desc);
		if (err != DDI_SUCCESS)
			goto fail;

		rr->rx_desc = (void *)rr->rx_dma_desc.re_db_va;
		rr->rx_desc_stride = desclen;
	}

	for (tx_done = 0; tx_done < sc->num_tx_rings; tx_done++) {
		re_tx_ring_t *tr = &sc->tx_rings[tx_done];

		err = re_alloc_tx_ring_desc(sc, &tr->tx_dma_desc);
		if (err != DDI_SUCCESS)
			goto fail;

		tr->tx_desc = (struct TxDesc *)(void *)tr->tx_dma_desc.re_db_va;
	}

	return (DDI_SUCCESS);

fail:
	for (int i = 0; i < rx_done; i++) {
		re_rx_ring_t *rr = &sc->rx_rings[i];
		re_dma_block_free(&rr->rx_dma_desc);
		rr->rx_desc = NULL;
	}

	for (int i = 0; i < tx_done; i++) {
		re_tx_ring_t *tr = &sc->tx_rings[i];
		re_dma_block_free(&tr->tx_dma_desc);
		tr->tx_desc = NULL;
	}

	return (DDI_FAILURE);
}

int
re_alloc_rx_buf(struct re_softc *sc, re_rx_buffer_t *rb, size_t len)
{
	int err;

	rb->re_rx_mp = NULL;
	rb->rrb_loaned = B_FALSE;
	bzero(&rb->rx_dma_buf, sizeof (rb->rx_dma_buf));

	err = re_dma_block_alloc(sc->dev, &rb->rx_dma_buf,
	    &re_rx_buf_dma_attr, &re_desc_accattr,
	    len,
	    DDI_DMA_RDWR | DDI_DMA_CONSISTENT,
	    DDI_DMA_CONSISTENT);
	if (err != DDI_SUCCESS)
		return (DDI_FAILURE);

	return (DDI_SUCCESS);
}

int
re_alloc_rx_bufs(struct re_softc *sc)
{
	int r, i, err;

	for (r = 0; r < sc->num_rx_rings; r++) {
		re_rx_ring_t *rr = &sc->rx_rings[r];

		if (rr->num_bufs < rr->num_desc)
			goto fail;

		rr->rx_bufs = kmem_zalloc(sizeof (re_rx_buffer_t) *
		    rr->num_bufs, KM_SLEEP);
		rr->rx_free_list = kmem_zalloc(sizeof (re_rx_buffer_t *) *
		    rr->num_bufs, KM_SLEEP);
		rr->rx_desc_bufs = kmem_zalloc(sizeof (re_rx_buffer_t *) *
		    rr->num_desc, KM_SLEEP);

		rr->rx_nfree = 0;
		rr->rx_cons = 0;
		rr->rx_loaned = 0;

		for (i = 0; i < rr->num_bufs; i++) {
			re_rx_buffer_t *rb = &rr->rx_bufs[i];

			rb->rx_ring = rr;
			rb->re_rx_mp = NULL;
			rb->rrb_loaned = B_FALSE;
			rb->rrb_free_rtn.free_func = re_rx_recycle;
			rb->rrb_free_rtn.free_arg = (caddr_t)rb;

			err = re_alloc_rx_buf(sc, rb, RE_JUMBO_FRAMELEN);
			if (err != DDI_SUCCESS)
				goto fail;

			err = re_rx_buf_create_mblk(rb);
			if (err != DDI_SUCCESS)
				goto fail;
		}
	}

	return (DDI_SUCCESS);

fail:
	re_free_rx_bufs(sc);
	return (DDI_FAILURE);
}

static void
re_rx_desc_set_addr(struct re_softc *sc, re_rx_ring_t *rr,
    uint32_t idx, uint64_t dma)
{
	void *p = (uint8_t *)rr->rx_desc + idx * rr->rx_desc_stride;

	switch (sc->re_rx_ring_desc_type) {
	case RE_RX_RING_DESC_TYPE_4: {
		struct RxDescV4 *d = p;
		d->addr = LE_64(dma);
		break;
	}
	case RE_RX_RING_DESC_TYPE_3: {
		struct RxDescV3 *d = p;
		d->addr = LE_64(dma);
		break;
	}
	default: {
		struct RxDesc *d = p;
		d->addr = LE_64(dma);
		break;
	}
	}
}

static void
re_rx_desc_set_opts1(struct re_softc *sc, re_rx_ring_t *rr,
    uint32_t idx, uint32_t opts1)
{
    void *p = (uint8_t *)rr->rx_desc + idx * rr->rx_desc_stride;

    switch (sc->re_rx_ring_desc_type) {
    case RE_RX_RING_DESC_TYPE_4: {
        struct RxDescV4 *d = p;
        d->RxDescNormalDDWord2.opts1 = LE_32(opts1);
        break;
    }
    case RE_RX_RING_DESC_TYPE_3: {
        struct RxDescV3 *d = p;
        d->RxDescNormalDDWord4.opts1 = LE_32(opts1);
        break;
    }
    default: {
        struct RxDesc *d = p;
        d->opts1 = LE_32(opts1);
        break;
    }
    }
}

static void
re_rx_desc_set_opts2(struct re_softc *sc, re_rx_ring_t *rr,
    uint32_t idx, uint32_t opts2)
{
    void *p = (uint8_t *)rr->rx_desc + idx * rr->rx_desc_stride;

    switch (sc->re_rx_ring_desc_type) {
    case RE_RX_RING_DESC_TYPE_4: {
        struct RxDescV4 *d = p;
        d->RxDescNormalDDWord2.opts2 = LE_32(opts2);
        break;
    }
    case RE_RX_RING_DESC_TYPE_3: {
        struct RxDescV3 *d = p;
        d->RxDescNormalDDWord4.opts2 = LE_32(opts2);
        break;
    }
    default: {
        struct RxDesc *d = p;
        d->opts2 = LE_32(opts2);
        break;
    }
    }
}

static uint32_t
re_rx_desc_get_opts1(struct re_softc *sc, re_rx_ring_t *rr, uint32_t idx)
{
	void *p = (uint8_t *)rr->rx_desc + idx * rr->rx_desc_stride;

	switch (sc->re_rx_ring_desc_type) {
	case RE_RX_RING_DESC_TYPE_4: {
		struct RxDescV4 *d = p;
		return (LE_32(d->RxDescNormalDDWord2.opts1));
	}
	case RE_RX_RING_DESC_TYPE_3: {
		struct RxDescV3 *d = p;
		return (LE_32(d->RxDescNormalDDWord4.opts1));
	}
	default: {
		struct RxDesc *d = p;
		return (LE_32(d->opts1));
	}
	}
}

static uint32_t
re_rx_desc_get_opts2(struct re_softc *sc, re_rx_ring_t *rr, uint32_t idx)
{
	void *p = (uint8_t *)rr->rx_desc + idx * rr->rx_desc_stride;

	switch (sc->re_rx_ring_desc_type) {
	case RE_RX_RING_DESC_TYPE_4: {
		struct RxDescV4 *d = p;
		return (LE_32(d->RxDescNormalDDWord2.opts2));
	}
	case RE_RX_RING_DESC_TYPE_3: {
		struct RxDescV3 *d = p;
		return (LE_32(d->RxDescNormalDDWord4.opts2));
	}
	default: {
		struct RxDesc *d = p;
		return (LE_32(d->opts2));
	}
	}
}

static void
re_map_rx_desc_to_dev(struct re_softc *sc, re_rx_ring_t *rr)
{
	uint64_t addr;

	addr = rr->rx_dma_desc.cookie.dmac_laddress;

	RE_WRITE_4(sc, rr->rdsar_reg, RE_ADDR_LO(addr));
	RE_WRITE_4(sc, rr->rdsar_reg + 4, RE_ADDR_HI(addr));
}

static void
re_rx_buf_sync(re_rx_buffer_t *rb, uint_t sync_flag)
{
#ifdef DEBUG
	ASSERT(rb != NULL);
	ASSERT(rb->rx_dma_buf.re_dma_hdl != NULL);
	ASSERT0(ddi_dma_sync(rb->rx_dma_buf.re_dma_hdl, 0,
	    rb->rx_dma_buf.re_db_alloc_len, sync_flag));
#else
	(void) ddi_dma_sync(rb->rx_dma_buf.re_dma_hdl, 0,
	    rb->rx_dma_buf.re_db_alloc_len, sync_flag);
#endif
}

int
re_rx_ring_fill(struct re_softc *sc, re_rx_ring_t *rr)
{
	uint32_t i;

	if (rr->num_bufs < rr->num_desc)
		return (DDI_FAILURE);

	re_rx_assembly_reset(rr);

	rr->rx_nfree = 0;
	rr->rx_cons = 0;
	rr->rx_loaned = 0;

	for (i = 0; i < rr->num_desc; i++) {
		re_rx_buffer_t *rb = &rr->rx_bufs[i];

		rr->rx_desc_bufs[i] = rb;
		re_rx_desc_rearm(sc, rr, i, rb);
	}

	for (; i < rr->num_bufs; i++) {
		re_rx_buffer_t *rb = &rr->rx_bufs[i];
		rr->rx_free_list[rr->rx_nfree++] = rb;
	}

	return (DDI_SUCCESS);
}

void
re_free_rx_buf(struct re_softc *sc, re_rx_buffer_t *rb)
{
	(void)sc;

	re_dma_block_free(&rb->rx_dma_buf);

	rb->rx_ring = NULL;
}

static void
re_free_rx_bufs(struct re_softc *sc)
{
	int r, i;

	if (sc == NULL)
		return;

	for (r = 0; r < sc->num_rx_rings; r++) {
		re_rx_ring_t *rr = &sc->rx_rings[r];

		if (rr == NULL)
			continue;

		re_rx_assembly_reset(rr);

		if (rr->rx_bufs != NULL) {
			for (i = 0; i < rr->num_bufs; i++) {
				re_rx_buffer_t *rb = &rr->rx_bufs[i];

				if (rb->re_rx_mp != NULL) {
					freemsg(rb->re_rx_mp);
					rb->re_rx_mp = NULL;
				}

				re_free_rx_buf(sc, rb);
			}

			kmem_free(rr->rx_bufs,
			    sizeof (re_rx_buffer_t) * rr->num_bufs);
			rr->rx_bufs = NULL;
		}

		if (rr->rx_free_list != NULL) {
			kmem_free(rr->rx_free_list,
			    sizeof (re_rx_buffer_t *) * rr->num_bufs);
			rr->rx_free_list = NULL;
		}

		if (rr->rx_desc_bufs != NULL) {
			kmem_free(rr->rx_desc_bufs,
			    sizeof (re_rx_buffer_t *) * rr->num_desc);
			rr->rx_desc_bufs = NULL;
		}

		rr->rx_nfree = 0;
		rr->rx_cons = 0;
	}
}

static void
re_rx_ring_init_masks(struct re_softc *sc, re_rx_ring_t *rr)
{
	switch (sc->re_rx_ring_desc_type) {
	case RE_RX_RING_DESC_TYPE_3:
		rr->desc_own_mask = DescOwn_V3;
		rr->desc_eor_mask = RingEnd_V3;
		rr->desc_sof_mask = FirstFrag_V3;
		rr->desc_eof_mask = LastFrag_V3;
		break;
	case RE_RX_RING_DESC_TYPE_4:
		rr->desc_own_mask = DescOwn_V4;
		rr->desc_eor_mask = RingEnd_V4;
		rr->desc_sof_mask = FirstFrag_V4;
		rr->desc_eof_mask = LastFrag_V4;
		break;
	default:
		rr->desc_own_mask = DescOwn;
		rr->desc_eor_mask = RingEnd;
		rr->desc_sof_mask = FirstFrag;
		rr->desc_eof_mask = LastFrag;
		break;
	}
}

int
re_init_rx_rings(struct re_softc *sc)
{
	int err;
	int i;

	err = re_alloc_rx_bufs(sc);
	if (err != DDI_SUCCESS)
		return (DDI_FAILURE);

	for (i = 0; i < sc->num_rx_rings; i++) {
		re_rx_ring_t *rr = &sc->rx_rings[i];

		re_rx_ring_init_masks(sc, rr);

		err = re_rx_ring_fill(sc, rr);
		if (err != DDI_SUCCESS)
			goto fail;
	}

	return (DDI_SUCCESS);

fail:
	re_free_rx_bufs(sc);
	return (DDI_FAILURE);
}

static int
re_rx_buf_create_mblk(re_rx_buffer_t *rb)
{
	caddr_t va;
	size_t len;

	va = rb->rx_dma_buf.re_db_va;
	len = rb->rx_dma_buf.re_db_alloc_len;

	rb->re_rx_mp = desballoc((uchar_t *)va, len, 0, &rb->rrb_free_rtn);
	if (rb->re_rx_mp == NULL)
		return (DDI_FAILURE);

	return (DDI_SUCCESS);
}

static void
re_rx_recycle(caddr_t arg)
{
	re_rx_buffer_t *rb = (re_rx_buffer_t *)arg;
	re_rx_ring_t *rr = rb->rx_ring;

	rb->re_rx_mp = NULL;

	if (!rb->rrb_loaned)
		return;

	rb->rrb_loaned = B_FALSE;

	if (re_rx_buf_create_mblk(rb) != DDI_SUCCESS) {
		mutex_enter(&rr->rx_free_lock);
		ASSERT(rr->rx_loaned != 0);
		rr->rx_loaned--;
		cv_signal(&rr->rx_free_cv);
		mutex_exit(&rr->rx_free_lock);
		return;
	}

	mutex_enter(&rr->rx_free_lock);
	ASSERT(rr->rx_loaned != 0);
	rr->rx_loaned--;
	rr->rx_free_list[rr->rx_nfree++] = rb;
	rr->rx_kstat.rx_recycled.value.ui64++;
	cv_signal(&rr->rx_free_cv);
	mutex_exit(&rr->rx_free_lock);
}

static re_rx_buffer_t *
re_rx_get_free_buf(re_rx_ring_t *rr)
{
	re_rx_buffer_t *rb = NULL;

	mutex_enter(&rr->rx_free_lock);
	if (rr->rx_nfree != 0)
		rb = rr->rx_free_list[--rr->rx_nfree];
	mutex_exit(&rr->rx_free_lock);

	return (rb);
}

static void
re_rx_put_free_buf(re_rx_ring_t *rr, re_rx_buffer_t *rb)
{
	mutex_enter(&rr->rx_free_lock);
	rr->rx_free_list[rr->rx_nfree++] = rb;
	mutex_exit(&rr->rx_free_lock);
}

static void
re_rx_desc_rearm(struct re_softc *sc, re_rx_ring_t *rr,
    uint32_t idx, re_rx_buffer_t *rb)
{
	uint32_t opts1;

	opts1 = ((uint32_t)rb->rx_dma_buf.re_db_alloc_len &
		RE_RDCMDSTS_FRAGLEN) | rr->desc_own_mask;

	if (idx == rr->num_desc - 1)
		opts1 |= rr->desc_eor_mask;

	re_rx_desc_set_addr(sc, rr, idx, rb->rx_dma_buf.cookie.dmac_laddress);
	re_rx_desc_set_opts2(sc, rr, idx, 0);

	membar_producer();
	re_rx_desc_set_opts1(sc, rr, idx, opts1);
	re_rx_desc_sync(rr, idx, DDI_DMA_SYNC_FORDEV);
	re_rx_buf_sync(rb, DDI_DMA_SYNC_FORDEV);
}

static int
re_rx_drain_ring(re_rx_ring_t *rr, clock_t ticks)
{
	int ret = 0;
	clock_t deadline = 0;

	mutex_enter(&rr->rx_free_lock);

	if (ticks > 0)
		deadline = ddi_get_lbolt() + ticks;

	while (rr->rx_loaned != 0) {
		if (ticks > 0) {
			if (cv_timedwait(&rr->rx_free_cv,
			    &rr->rx_free_lock, deadline) == -1) {
				ret = ETIME;
				break;
			}
		} else {
			cv_wait(&rr->rx_free_cv, &rr->rx_free_lock);
		}
	}

	mutex_exit(&rr->rx_free_lock);
	return (ret);
}

static int
re_rx_drain(struct re_softc *sc, clock_t ticks)
{
	int i, ret;

	for (i = 0; i < sc->num_rx_rings; i++) {
		ret = re_rx_drain_ring(&sc->rx_rings[i], ticks);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static void
re_rx_desc_sync(re_rx_ring_t *rr, uint32_t idx, uint_t sync_flag)
{
	off_t off;

	off = (off_t)idx * rr->rx_desc_stride;

#ifdef DEBUG
	ASSERT0(ddi_dma_sync(rr->rx_dma_desc.re_dma_hdl,
	    off, rr->rx_desc_stride, sync_flag));
#else
	(void) ddi_dma_sync(rr->rx_dma_desc.re_dma_hdl,
	    off, rr->rx_desc_stride, sync_flag);
#endif
}

static mblk_t *
re_rx_copy(re_rx_buffer_t *rb, size_t pkt_len)
{
	mblk_t *mp;

	re_rx_buf_sync(rb, DDI_DMA_SYNC_FORKERNEL);

	mp = allocb(pkt_len, 0);
	if (mp == NULL)
		return (NULL);

	bcopy(rb->rx_dma_buf.re_db_va, mp->b_wptr, pkt_len);
	mp->b_wptr += pkt_len;

	return (mp);
}

static mblk_t *
re_rx_bind(re_rx_buffer_t *rb, size_t pkt_len)
{
	mblk_t *mp;
	re_rx_ring_t *rr = rb->rx_ring;

	re_rx_buf_sync(rb, DDI_DMA_SYNC_FORKERNEL);

	mp = rb->re_rx_mp;
	rb->re_rx_mp = NULL;
	rb->rrb_loaned = B_TRUE;

	mutex_enter(&rr->rx_free_lock);
	rr->rx_loaned++;
	mutex_exit(&rr->rx_free_lock);

	mp->b_rptr = (uchar_t *)rb->rx_dma_buf.re_db_va;
	mp->b_wptr = mp->b_rptr + pkt_len;
	mp->b_next = NULL;
	mp->b_prev = NULL;
	mp->b_cont = NULL;

	return (mp);
}

static void
re_rx_assembly_reset(re_rx_ring_t *rr)
{
	if (rr->rrx_assem_head != NULL) {
		freemsgchain(rr->rrx_assem_head);
		rr->rrx_assem_head = NULL;
		rr->rrx_assem_tail = NULL;
	}

	rr->rrx_assem_bytes = 0;
	rr->rrx_ip_csum_valid = B_FALSE;
	rr->rrx_ip_csum_ok = B_FALSE;
	rr->rrx_l4_csum_valid = B_FALSE;
	rr->rrx_l4_csum_ok = B_FALSE;
}

static void
re_rx_assembly_state_clear(mblk_t **pkt_head, mblk_t **pkt_tail,
    uint32_t *pkt_bytes,
    boolean_t *ip_valid, boolean_t *ip_ok,
    boolean_t *l4_valid, boolean_t *l4_ok)
{
	*pkt_head = NULL;
	*pkt_tail = NULL;
	*pkt_bytes = 0;
	*ip_valid = B_FALSE;
	*ip_ok = B_FALSE;
	*l4_valid = B_FALSE;
	*l4_ok = B_FALSE;
}

static void
re_rx_csum_info(struct re_softc *sc, uint32_t opts1, uint32_t opts2,
    boolean_t *ip_valid, boolean_t *ip_ok,
    boolean_t *l4_valid, boolean_t *l4_ok)
{
	uint32_t csum_bits;

	*ip_valid = B_FALSE;
	*ip_ok = B_FALSE;
	*l4_valid = B_FALSE;
	*l4_ok = B_FALSE;

	switch (sc->re_rx_ring_desc_type) {
	case RE_RX_RING_DESC_TYPE_3:
		csum_bits = opts2;

		if ((csum_bits & RE_RX_TCPT_V3) != 0) {
			*ip_valid = B_TRUE;
			*ip_ok = ((csum_bits & RE_RX_IPF_V3) == 0);
			*l4_valid = B_TRUE;
			*l4_ok = ((csum_bits & RE_RX_TCPF_V3) == 0);
		} else if ((csum_bits & RE_RX_UDPT_V3) != 0) {
			*ip_valid = B_TRUE;
			*ip_ok = ((csum_bits & RE_RX_IPF_V3) == 0);
			*l4_valid = B_TRUE;
			*l4_ok = ((csum_bits & RE_RX_UDPF_V3) == 0);
		}
		break;

	case RE_RX_RING_DESC_TYPE_4:
		csum_bits = opts1;

		if ((csum_bits & RE_RX_TCPT_V4) != 0) {
			*ip_valid = B_TRUE;
			*ip_ok = ((csum_bits & RE_RX_IPF_V4) == 0);
			*l4_valid = B_TRUE;
			*l4_ok = ((csum_bits & RE_RX_TCPF_V4) == 0);
		} else if ((csum_bits & RE_RX_UDPT_V4) != 0) {
			*ip_valid = B_TRUE;
			*ip_ok = ((csum_bits & RE_RX_IPF_V4) == 0);
			*l4_valid = B_TRUE;
			*l4_ok = ((csum_bits & RE_RX_UDPF_V4) == 0);
		}
		break;

	default:
		csum_bits = opts1;

		if ((csum_bits & RE_RX_TCPT) != 0) {
			*ip_valid = B_TRUE;
			*ip_ok = ((csum_bits & RE_RX_IPF) == 0);
			*l4_valid = B_TRUE;
			*l4_ok = ((csum_bits & RE_RX_TCPF) == 0);
		} else if ((csum_bits & RE_RX_UDPT) != 0) {
			*ip_valid = B_TRUE;
			*ip_ok = ((csum_bits & RE_RX_IPF) == 0);
			*l4_valid = B_TRUE;
			*l4_ok = ((csum_bits & RE_RX_UDPF) == 0);
		}
		break;
	}
}

mblk_t *
re_ring_rx(re_rx_ring_t *rr, int poll_bytes)
{
	struct re_softc *sc = rr->sc;
	mblk_t *head = NULL;
	mblk_t **tailp = &head;
	mblk_t *pkt_head, *pkt_tail;
	uint32_t idx = rr->rx_cons;
	uint32_t pkts_done = 0;
	int bytes_done = 0;
	uint32_t pkt_bytes;
	boolean_t any_rx = B_FALSE;

	boolean_t pkt_ip_csum_valid;
	boolean_t pkt_ip_csum_ok;
	boolean_t pkt_l4_csum_valid;
	boolean_t pkt_l4_csum_ok;

	ASSERT(rr != NULL);
	ASSERT(sc != NULL);

	if (sc->sc_stopped)
		return (NULL);

	/*
	 * Make device updates to descriptor ring visible to CPU
	 * before we start reading descriptors.
	 */
#ifdef DEBUG
	ASSERT0(ddi_dma_sync(rr->rx_dma_desc.re_dma_hdl, 0,
	    rr->rx_dma_desc.re_db_alloc_len, DDI_DMA_SYNC_FORKERNEL));
#else
	(void) ddi_dma_sync(rr->rx_dma_desc.re_dma_hdl, 0,
	    rr->rx_dma_desc.re_db_alloc_len, DDI_DMA_SYNC_FORKERNEL);
#endif

	pkt_head = rr->rrx_assem_head;
	pkt_tail = rr->rrx_assem_tail;
	pkt_bytes = rr->rrx_assem_bytes;
	pkt_ip_csum_valid = rr->rrx_ip_csum_valid;
	pkt_ip_csum_ok = rr->rrx_ip_csum_ok;
	pkt_l4_csum_valid = rr->rrx_l4_csum_valid;
	pkt_l4_csum_ok = rr->rrx_l4_csum_ok;

	for (;;) {
		re_rx_buffer_t *rb, *new_rb = NULL;
		mblk_t *mp = NULL;
		uint32_t opts1, opts2;
		size_t raw_len, frag_len;
		boolean_t sof, eof;
		boolean_t use_bind = B_FALSE;
		boolean_t have_assembly;
		boolean_t single_desc;

		/*
		 * Per-desc sync is still useful if descriptor layout is updated
		 * individually, but whole-ring sync above is the critical one.
		 */
		re_rx_desc_sync(rr, idx, DDI_DMA_SYNC_FORKERNEL);

		opts1 = re_rx_desc_get_opts1(sc, rr, idx);

		if ((opts1 & rr->desc_own_mask) != 0)
			break;

		/*
		 * OWN is clear, so make sure the rest of descriptor contents
		 * are visible before we read them.
		 */
		membar_consumer();

		opts2 = re_rx_desc_get_opts2(sc, rr, idx);
		rb = rr->rx_desc_bufs[idx];
		sof = ((opts1 & rr->desc_sof_mask) != 0);
		eof = ((opts1 & rr->desc_eof_mask) != 0);
		have_assembly = (pkt_head != NULL);
		any_rx = B_TRUE;

		if (!have_assembly && !sof) {
			if (!eof)
				rr->rx_kstat.rx_no_sof_no_eof.value.ui64++;
			rr->rx_kstat.rx_desc_err.value.ui64++;
			re_rx_desc_rearm(sc, rr, idx, rb);
			goto next;
		}

		if (have_assembly && sof) {
			rr->rx_kstat.rx_restart_assembly.value.ui64++;
			freemsgchain(pkt_head);
			re_rx_assembly_state_clear(&pkt_head, &pkt_tail,
			    &pkt_bytes,
			    &pkt_ip_csum_valid, &pkt_ip_csum_ok,
			    &pkt_l4_csum_valid, &pkt_l4_csum_ok);
			have_assembly = B_FALSE;
		}

		raw_len = (size_t)(opts1 & RE_RDCMDSTS_FRAGLEN);
		if (raw_len == 0 || raw_len > rb->rx_dma_buf.re_db_alloc_len) {
			rr->rx_kstat.rx_desc_err.value.ui64++;

			if (pkt_head != NULL) {
				freemsgchain(pkt_head);
				re_rx_assembly_state_clear(&pkt_head, &pkt_tail,
				    &pkt_bytes,
				    &pkt_ip_csum_valid, &pkt_ip_csum_ok,
				    &pkt_l4_csum_valid, &pkt_l4_csum_ok);
			}

			re_rx_desc_rearm(sc, rr, idx, rb);
			goto next;
		}

		frag_len = raw_len;
		if (eof) {
			if (frag_len < ETHERFCSL) {
				rr->rx_kstat.rx_desc_err.value.ui64++;

				if (pkt_head != NULL) {
					freemsgchain(pkt_head);
					re_rx_assembly_state_clear(&pkt_head,
					    &pkt_tail, &pkt_bytes,
					    &pkt_ip_csum_valid,
					    &pkt_ip_csum_ok,
					    &pkt_l4_csum_valid,
					    &pkt_l4_csum_ok);
				}

				re_rx_desc_rearm(sc, rr, idx, rb);
				goto next;
			}
			frag_len -= ETHERFCSL;
		}

		if (poll_bytes > 0 &&
		    pkt_head == NULL &&
		    bytes_done != 0 &&
		    (bytes_done + (int)frag_len) > poll_bytes)
			break;

		if (!rb->rrb_loaned && rb->re_rx_mp != NULL) {
			new_rb = re_rx_get_free_buf(rr);
			if (new_rb != NULL) {
				mp = re_rx_bind(rb, frag_len);
				if (mp != NULL) {
					use_bind = B_TRUE;
				} else {
					rr->rx_kstat.rx_bind_fail.value.ui64++;
					re_rx_put_free_buf(rr, new_rb);
					new_rb = NULL;
				}
			}
		}

		if (use_bind) {
			rr->rx_kstat.rx_bind_pkts.value.ui64++;
			rr->rx_desc_bufs[idx] = new_rb;
			re_rx_desc_rearm(sc, rr, idx, new_rb);
		} else {
			mp = re_rx_copy(rb, frag_len);
			if (mp == NULL) {
				rr->rx_kstat.rx_allocb_fail.value.ui64++;

				if (pkt_head != NULL) {
					freemsgchain(pkt_head);
					re_rx_assembly_state_clear(&pkt_head,
					    &pkt_tail, &pkt_bytes,
					    &pkt_ip_csum_valid,
					    &pkt_ip_csum_ok,
					    &pkt_l4_csum_valid,
					    &pkt_l4_csum_ok);
				}

				re_rx_desc_rearm(sc, rr, idx, rb);
				goto next;
			}
			rr->rx_kstat.rx_copy_pkts.value.ui64++;
			re_rx_desc_rearm(sc, rr, idx, rb);
		}

		mp->b_next = NULL;
		mp->b_prev = NULL;
		mp->b_cont = NULL;

		if (!have_assembly) {
			re_rx_csum_info(sc, opts1, opts2,
			    &pkt_ip_csum_valid, &pkt_ip_csum_ok,
			    &pkt_l4_csum_valid, &pkt_l4_csum_ok);
		}

		single_desc = (sof && eof);

		if (single_desc && !have_assembly) {
			uint32_t csum_flags = 0;

			if (pkt_ip_csum_valid && pkt_ip_csum_ok)
				csum_flags |= HCK_IPV4_HDRCKSUM_OK;
			if (pkt_l4_csum_valid && pkt_l4_csum_ok)
				csum_flags |= HCK_FULLCKSUM_OK;

			if (csum_flags != 0)
				mac_hcksum_set(mp, 0, 0, 0, 0, csum_flags);

			rr->rx_kstat.rx_single_desc_pkts.value.ui64++;
			rr->rx_kstat.ipackets64.value.ui64++;
			rr->rx_kstat.rbytes64.value.ui64 += frag_len;

			pkts_done++;
			bytes_done += (int)frag_len;

			*tailp = mp;
			tailp = &mp->b_next;


			if (poll_bytes > 0 && bytes_done >= poll_bytes)
				goto done;

			goto next;
		}

		if (!have_assembly) {
			pkt_head = pkt_tail = mp;
			pkt_bytes = (uint32_t)frag_len;
		} else {
			pkt_tail->b_cont = mp;
			pkt_tail = mp;
			pkt_bytes += (uint32_t)frag_len;
		}

		if (!eof) {
			rr->rx_kstat.rx_fragment_pkts.value.ui64++;
			goto next;
		}

		{
			mblk_t *done_mp = pkt_head;
			uint32_t csum_flags = 0;

			if (pkt_ip_csum_valid && pkt_ip_csum_ok)
				csum_flags |= HCK_IPV4_HDRCKSUM_OK;
			if (pkt_l4_csum_valid && pkt_l4_csum_ok)
				csum_flags |= HCK_FULLCKSUM_OK;

			if (csum_flags != 0)
				mac_hcksum_set(done_mp, 0, 0, 0, 0, csum_flags);

			done_mp->b_next = NULL;
			done_mp->b_prev = NULL;

			rr->rx_kstat.ipackets64.value.ui64++;
			rr->rx_kstat.rbytes64.value.ui64 += pkt_bytes;

			pkts_done++;
			bytes_done += (int)pkt_bytes;

			*tailp = done_mp;
			tailp = &done_mp->b_next;

			re_rx_assembly_state_clear(&pkt_head, &pkt_tail,
			    &pkt_bytes,
			    &pkt_ip_csum_valid, &pkt_ip_csum_ok,
			    &pkt_l4_csum_valid, &pkt_l4_csum_ok);

			if (poll_bytes > 0 && bytes_done >= poll_bytes)
				goto done;
		}

next:
		idx++;
		if (idx == rr->num_desc)
			idx = 0;
	}

done:
	rr->rrx_assem_head = pkt_head;
	rr->rrx_assem_tail = pkt_tail;
	rr->rrx_assem_bytes = pkt_bytes;
	rr->rrx_ip_csum_valid = pkt_ip_csum_valid;
	rr->rrx_ip_csum_ok = pkt_ip_csum_ok;
	rr->rrx_l4_csum_valid = pkt_l4_csum_valid;
	rr->rrx_l4_csum_ok = pkt_l4_csum_ok;

	rr->rx_cons = idx;

	/*
	 * Make descriptor updates visible to device after rearming.
	 */
	if (any_rx) {
#ifdef DEBUG
		ASSERT0(ddi_dma_sync(rr->rx_dma_desc.re_dma_hdl, 0,
		    rr->rx_dma_desc.re_db_alloc_len, DDI_DMA_SYNC_FORDEV));
#else
		(void) ddi_dma_sync(rr->rx_dma_desc.re_dma_hdl, 0,
		    rr->rx_dma_desc.re_db_alloc_len, DDI_DMA_SYNC_FORDEV);
#endif
		membar_producer();
	}

	*tailp = NULL;

	return (head);
}

/* end of rx */

static void
re_map_tx_desc_to_dev(struct re_softc *sc, re_tx_ring_t *tr)
{
	uint64_t addr;

	addr = tr->tx_dma_desc.cookie.dmac_laddress;

	RE_WRITE_4(sc, tr->tdsar_reg, RE_ADDR_LO(addr));
	RE_WRITE_4(sc, tr->tdsar_reg + 4, RE_ADDR_HI(addr));
}

static int
re_alloc_tx_bufs(struct re_softc *sc, re_tx_ring_t *tr)
{
	uint32_t i = 0;
	size_t tx_copy_len;
	int err;

	if (tr->num_bufs == 0)
		tr->num_bufs = tr->num_desc;

	tr->tx_work_list = kmem_zalloc(sizeof (re_tx_buffer_t *) *
	    tr->num_desc, KM_SLEEP);
	if (tr->tx_work_list == NULL)
		goto fail_preloop;

	tr->tx_arena = kmem_zalloc(sizeof (re_tx_buffer_t) *
	    tr->num_bufs, KM_SLEEP);
	if (tr->tx_arena == NULL)
		goto fail_preloop;

	list_create(&tr->tx_free_list, sizeof (re_tx_buffer_t),
	    offsetof(re_tx_buffer_t, tx_node));
	tr->tx_free_list_init = B_TRUE;

	tx_copy_len = RE_JUMBO_FRAMELEN;

	for (i = 0; i < tr->num_bufs; i++) {
		re_tx_buffer_t *tb = &tr->tx_arena[i];
		uint_t ccount = 0;

		tb->tx_ring = tr;
		tb->re_tx_mp = NULL;
		tb->tx_ncookies = 0;
		tb->tx_ndescs = 0;
		tb->tx_last_idx = 0;
		tb->tx_bound = B_FALSE;
		tb->tx_copied = B_FALSE;
		tb->tx_bind_hdl = NULL;

		bzero(tb->tx_cookies, sizeof (tb->tx_cookies));
		bzero(&tb->tx_copy_buf, sizeof (tb->tx_copy_buf));

		err = ddi_dma_alloc_handle(sc->dev,
		    (ddi_dma_attr_t *)&re_tx_copy_dma_attr,
		    DDI_DMA_SLEEP, NULL, &tb->tx_copy_buf.re_dma_hdl);
		if (err != DDI_SUCCESS)
			goto fail_loop;

		err = ddi_dma_mem_alloc(tb->tx_copy_buf.re_dma_hdl,
		    tx_copy_len, &re_buf_accattr,
		    DDI_DMA_STREAMING, DDI_DMA_SLEEP, NULL,
		    &tb->tx_copy_buf.re_db_va,
		    &tb->tx_copy_buf.re_db_alloc_len,
		    &tb->tx_copy_buf.re_acc_hdl);
		if (err != DDI_SUCCESS)
			goto fail_loop;

		err = ddi_dma_addr_bind_handle(tb->tx_copy_buf.re_dma_hdl, NULL,
		    tb->tx_copy_buf.re_db_va,
		    tb->tx_copy_buf.re_db_alloc_len,
		    DDI_DMA_WRITE | DDI_DMA_STREAMING,
		    DDI_DMA_SLEEP, NULL,
		    &tb->tx_copy_buf.cookie, &ccount);
		if (err != DDI_DMA_MAPPED || ccount != 1)
			goto fail_loop;

		tb->tx_copy_buf.ncookies = ccount;
		tb->tx_copy_buf.re_bound = B_TRUE;

		err = ddi_dma_alloc_handle(sc->dev,
		    (ddi_dma_attr_t *)&re_tx_buf_dma_attr,
		    DDI_DMA_SLEEP, NULL, &tb->tx_bind_hdl);
		if (err != DDI_SUCCESS)
			goto fail_loop;

		list_insert_tail(&tr->tx_free_list, tb);
	}

	return (DDI_SUCCESS);

fail_loop:
	re_free_tx_bufs(tr);
	return (DDI_FAILURE);

fail_preloop:
	re_free_tx_bufs(tr);
	return (DDI_FAILURE);
}

void
re_drain_tx_ring(re_tx_ring_t *tr)
{
	uint32_t i;

	mutex_enter(&tr->tx_lock);

	if (tr->tx_work_list != NULL) {
		for (i = 0; i < tr->num_desc; i++) {
			re_tx_buffer_t *tb = tr->tx_work_list[i];

			if (tb == NULL)
				continue;

			tr->tx_work_list[i] = NULL;
			re_tx_buf_recycle(tb);
			list_insert_tail(&tr->tx_free_list, tb);
		}
	}

	tr->cur_tx = 0;
	tr->dirty_tx = 0;

	mutex_exit(&tr->tx_lock);
}

static void
re_free_tx_bufs(re_tx_ring_t *tr)
{
	uint32_t i;

	if (tr == NULL)
		return;

	if (tr->tx_lock_init)
		re_drain_tx_ring(tr);

	if (tr->tx_arena != NULL) {
		for (i = 0; i < tr->num_bufs; i++) {
			re_tx_buffer_t *tb = &tr->tx_arena[i];

			if (tb->tx_bind_hdl != NULL) {
				ddi_dma_free_handle(&tb->tx_bind_hdl);
				tb->tx_bind_hdl = NULL;
			}

			if (tb->tx_copy_buf.re_dma_hdl != NULL)
				re_dma_block_free(&tb->tx_copy_buf);
		}

		kmem_free(tr->tx_arena,
		    sizeof (re_tx_buffer_t) * tr->num_bufs);
		tr->tx_arena = NULL;
	}

	if (tr->tx_work_list != NULL) {
		kmem_free(tr->tx_work_list,
		    sizeof (re_tx_buffer_t *) * tr->num_desc);
		tr->tx_work_list = NULL;
	}

	if (tr->tx_free_list_init) {
		list_destroy(&tr->tx_free_list);
		tr->tx_free_list_init = B_FALSE;
	}
}

int
re_init_tx_rings(struct re_softc *sc)
{
	int i;


	for (i = 0; i < sc->num_tx_rings; i++) {
		re_tx_ring_t *tr = &sc->tx_rings[i];

		tr->num_bufs = tr->num_desc;
		tr->cur_tx = 0;
		tr->dirty_tx = 0;

		re_tx_ring_init_masks(tr);

		if (re_alloc_tx_bufs(sc, tr) != DDI_SUCCESS)
			goto fail;

		re_tx_desc_init_ring(tr);
	}

	return (DDI_SUCCESS);

fail:
	while (--i >= 0)
		re_free_tx_bufs(&sc->tx_rings[i]);
	return (DDI_FAILURE);
}

static void
re_free_tx_rings_bufs(struct re_softc *sc)
{
	int i;

	if (sc == NULL || sc->tx_rings == NULL)
		return;

	for (i = 0; i < sc->num_tx_rings; i++)
		re_free_tx_bufs(&sc->tx_rings[i]);
}

static uint32_t
re_tx_next(re_tx_ring_t *tr, uint32_t idx)
{
	idx++;
	if (idx == tr->num_desc)
		idx = 0;
	return (idx);
}

static size_t
re_msg_size(mblk_t *mp)
{
	size_t len = 0;

	for (; mp != NULL; mp = mp->b_cont)
		len += MBLKL(mp);

	return (len);
}

static boolean_t
re_mblk_copy_data(mblk_t *mp, size_t off, void *dst, size_t len)
{
	mblk_t *cur = mp;
	size_t skip = off;
	uint8_t *p = dst;

	while (cur != NULL && skip >= MBLKL(cur)) {
		skip -= MBLKL(cur);
		cur = cur->b_cont;
	}

	while (cur != NULL && len != 0) {
		size_t avail = MBLKL(cur) - skip;
		size_t n = MIN(avail, len);

		bcopy(cur->b_rptr + skip, p, n);

		p += n;
		len -= n;
		cur = cur->b_cont;
		skip = 0;
	}

	return (len == 0);
}

static uint32_t
re_get_patch_pad_len(struct re_softc *sc, mblk_t *mp,
    const re_tx_plan_t *plan, size_t pkt_len)
{
	uint32_t pad_len = 0;
	uint32_t hdr_len = 0;
	int trans_data_len;
	uint16_t dest_port;

	if (sc->re_type != MAC_R25 && sc->re_type != MAC_R25B)
		goto out;

	if (!(plan->is_tcp || plan->is_udp))
		goto out;

	if (plan->l4_offset == 0 || pkt_len >= RE_PATCH_PKT_MAX)
		goto out;

	if (pkt_len < plan->l4_offset)
		goto out;

	trans_data_len = (int)pkt_len - (int)plan->l4_offset;

	/*
	 * Linux vendor logic:
	 * UDP special-case for PTP (319/320).
	 */
	if (plan->is_udp) {
		if (trans_data_len > 3 && trans_data_len < RE_MIN_PATCH_LEN) {
			if (re_mblk_copy_data(mp, plan->l4_offset + 2,
			    &dest_port, sizeof (dest_port))) {
				dest_port = ntohs(dest_port);

				if (dest_port == 319 || dest_port == 320) {
					pad_len = RE_MIN_PATCH_LEN -
					    trans_data_len;
					goto out;
				}
			}
		}
		hdr_len = 8;
	} else if (plan->is_tcp) {
		hdr_len = 20;
	}

	if (trans_data_len < (int)hdr_len)
		pad_len = hdr_len - trans_data_len;

out:
	if ((pkt_len + pad_len) < ETHERMIN)
		pad_len = ETHERMIN - pkt_len;

	return (pad_len);
}

static int
re_mblk_append_zeros(mblk_t **mpp, size_t pad_len)
{
	mblk_t *mp, *tail, *prev;
	mblk_t *newtail;
	size_t old_len;

	mp = *mpp;

	if (pad_len == 0)
		return (DDI_SUCCESS);

	prev = NULL;
	for (tail = mp; tail->b_cont != NULL; tail = tail->b_cont)
		prev = tail;

	if ((size_t)(DB_LIM(tail) - tail->b_wptr) >= pad_len) {
		bzero(tail->b_wptr, pad_len);
		tail->b_wptr += pad_len;
		return (DDI_SUCCESS);
	}

	old_len = MBLKL(tail);
	newtail = reallocb(tail, old_len + pad_len, 1);
	if (newtail != NULL) {
		if (prev != NULL)
			prev->b_cont = newtail;
		else
			*mpp = newtail;

		bzero(newtail->b_rptr + old_len, pad_len);
		newtail->b_wptr += pad_len;
		return (DDI_SUCCESS);
	}

	newtail = allocb(pad_len, BPRI_MED);
	if (newtail == NULL)
		return (DDI_FAILURE);

	bzero(newtail->b_rptr, pad_len);
	newtail->b_wptr += pad_len;
	linkb(mp, newtail);

	return (DDI_SUCCESS);
}

static void
re_tx_ring_init_masks(re_tx_ring_t *tr)
{
	tr->desc_own_mask = DescOwn;
	tr->desc_eor_mask = RingEnd;
	tr->desc_sof_mask = FirstFrag;
	tr->desc_eof_mask = LastFrag;
}

static void
re_tx_desc_sync(re_tx_ring_t *tr, uint32_t idx, uint_t sync_flag)
{
	off_t off = (off_t)idx * sizeof (struct TxDesc);

#ifdef DEBUG
	ASSERT0(ddi_dma_sync(tr->tx_dma_desc.re_dma_hdl,
	    off, sizeof (struct TxDesc), sync_flag));
#else
	(void) ddi_dma_sync(tr->tx_dma_desc.re_dma_hdl,
	    off, sizeof (struct TxDesc), sync_flag);
#endif
}

static void
re_tx_ring_sync(re_tx_ring_t *tr, uint_t sync_flag)
{
#ifdef DEBUG
	ASSERT0(ddi_dma_sync(tr->tx_dma_desc.re_dma_hdl, 0,
	    tr->tx_dma_desc.re_db_alloc_len, sync_flag));
#else
	(void) ddi_dma_sync(tr->tx_dma_desc.re_dma_hdl, 0,
	    tr->tx_dma_desc.re_db_alloc_len, sync_flag);
#endif
}

static uint32_t
re_tx_desc_get_opts1(re_tx_ring_t *tr, uint32_t idx)
{
	re_tx_desc_sync(tr, idx, DDI_DMA_SYNC_FORKERNEL);
	return (LE_32(tr->tx_desc[idx].opts1));
}

static boolean_t
re_tx_desc_done(re_tx_ring_t *tr, uint32_t idx)
{
	uint32_t opts1;

	opts1 = re_tx_desc_get_opts1(tr, idx);
	return ((opts1 & tr->desc_own_mask) == 0);
}

void
re_tx_desc_init_ring(re_tx_ring_t *tr)
{
	uint32_t i;

	bzero(tr->tx_desc, sizeof (struct TxDesc) * tr->num_desc);

	for (i = 0; i < tr->num_desc; i++) {
		uint32_t opts1 = 0;

		if (i == tr->num_desc - 1)
			opts1 |= tr->desc_eor_mask;

		tr->tx_desc[i].opts1 = LE_32(opts1);
		tr->tx_desc[i].opts2 = 0;
		tr->tx_desc[i].addr = 0;
	}

	re_tx_ring_sync(tr, DDI_DMA_SYNC_FORDEV);
}

static void
re_tx_desc_write(re_tx_ring_t *tr, uint32_t idx, uint64_t addr,
    uint32_t opts1, uint32_t opts2)
{
	struct TxDesc *txd = &tr->tx_desc[idx];

	txd->addr = LE_64(addr);
	txd->opts2 = LE_32(opts2);
	txd->opts1 = LE_32(opts1);

	re_tx_desc_sync(tr, idx, DDI_DMA_SYNC_FORDEV);
}

uint32_t
re_tx_desc_free(re_tx_ring_t *tr)
{
	if (tr->cur_tx >= tr->dirty_tx)
		return (tr->num_desc - (tr->cur_tx - tr->dirty_tx) - 1);

	return (tr->dirty_tx - tr->cur_tx - 1);
}

static boolean_t
re_tx_desc_avail(re_tx_ring_t *tr, uint32_t need)
{
	return (re_tx_desc_free(tr) >= need);
}

static re_tx_buffer_t *
re_tx_get_free_buf(re_tx_ring_t *tr)
{
	re_tx_buffer_t *tb;

	mutex_enter(&tr->tx_lock);
	tb = list_is_empty(&tr->tx_free_list) ? NULL :
	    list_remove_head(&tr->tx_free_list);
	mutex_exit(&tr->tx_lock);

	return (tb);
}

/*static void
re_tx_put_free_buf(re_tx_ring_t *tr, re_tx_buffer_t *tb)
{
	re_tx_buf_recycle(tb);

	mutex_enter(&tr->tx_lock);
	list_insert_tail(&tr->tx_free_list, tb);
	mutex_exit(&tr->tx_lock);
}*/

static void
re_tx_buf_abort(re_tx_ring_t *tr, re_tx_buffer_t *tb)
{
	if (tb->tx_bound && tb->tx_bind_hdl != NULL) {
		(void) ddi_dma_unbind_handle(tb->tx_bind_hdl);
		tb->tx_bound = B_FALSE;
	}

	tb->re_tx_mp = NULL;
	tb->tx_ncookies = 0;
	tb->tx_ndescs = 0;
	tb->tx_last_idx = 0;
	tb->tx_copied = B_FALSE;
	bzero(tb->tx_cookies, sizeof (tb->tx_cookies));

	mutex_enter(&tr->tx_lock);
	list_insert_tail(&tr->tx_free_list, tb);
	mutex_exit(&tr->tx_lock);
}

static int
re_tx_bind_block(re_tx_buffer_t *tb, unsigned char *addr, size_t len)
{
	int err;
	uint_t ncookies;
	ddi_dma_cookie_t cookie;
	uint_t i;

	err = ddi_dma_addr_bind_handle(tb->tx_bind_hdl, NULL,
	    (caddr_t)addr, len,
	    DDI_DMA_WRITE | DDI_DMA_STREAMING,
	    DDI_DMA_SLEEP, NULL, &cookie, &ncookies);
	if (err != DDI_DMA_MAPPED)
		return (DDI_FAILURE);

	if (ncookies == 0 || ncookies > RE_NTXSEGS) {
		(void) ddi_dma_unbind_handle(tb->tx_bind_hdl);
		return (DDI_FAILURE);
	}

	tb->tx_cookies[0] = cookie;
	tb->tx_ncookies = ncookies;
	tb->tx_bound = B_TRUE;
	tb->tx_copied = B_FALSE;

	for (i = 1; i < ncookies; i++)
		ddi_dma_nextcookie(tb->tx_bind_hdl, &tb->tx_cookies[i]);

	return (DDI_SUCCESS);
}

static int
re_tx_bind_mblk_chain(re_tx_buffer_t *tb, mblk_t *mp)
{
	mblk_t *seg = NULL;
	mblk_t *cur;

	for (cur = mp; cur != NULL; cur = cur->b_cont) {
		if (MBLKL(cur) == 0)
			continue;

		if (seg != NULL)
			return (DDI_FAILURE);   /* больше одного data block -> copy */

		seg = cur;
	}

	if (seg == NULL)
		return (DDI_FAILURE);

	if (re_tx_bind_block(tb, (unsigned char *)seg->b_rptr,
	    MBLKL(seg)) != DDI_SUCCESS)
		return (DDI_FAILURE);

	tb->re_tx_mp = mp;
	return (DDI_SUCCESS);
}

static int
re_tx_copy_mblk(re_tx_buffer_t *tb, mblk_t *mp)
{
	size_t pkt_len;
	size_t off = 0;
	mblk_t *cur;

	pkt_len = re_msg_size(mp);
	if (pkt_len == 0)
		return (DDI_FAILURE);

	if (pkt_len > tb->tx_copy_buf.re_db_alloc_len)
		return (DDI_FAILURE);

	for (cur = mp; cur != NULL; cur = cur->b_cont) {
		size_t len = MBLKL(cur);

		if (len == 0)
			continue;

		bcopy(cur->b_rptr, tb->tx_copy_buf.re_db_va + off, len);
		off += len;
	}

#ifdef DEBUG
	ASSERT0(ddi_dma_sync(tb->tx_copy_buf.re_dma_hdl, 0,
	    pkt_len, DDI_DMA_SYNC_FORDEV));
#else
	(void) ddi_dma_sync(tb->tx_copy_buf.re_dma_hdl, 0,
	    pkt_len, DDI_DMA_SYNC_FORDEV);
#endif

	tb->re_tx_mp = mp;
	tb->tx_ncookies = 1;
	tb->tx_cookies[0] = tb->tx_copy_buf.cookie;
	tb->tx_cookies[0].dmac_size = pkt_len;
	tb->tx_bound = B_FALSE;
	tb->tx_copied = B_TRUE;

	return (DDI_SUCCESS);
}

static boolean_t
re_tx_prepare_plan(mblk_t *mp, re_tx_plan_t *plan)
{
	mac_ether_offload_info_t meoi;
	uint32_t req;

	bzero(plan, sizeof (*plan));
	bzero(&meoi, sizeof (meoi));

	plan->min_pkt_len = RE_MIN_FRAMELEN;

	mac_hcksum_get(mp, NULL, NULL, NULL, NULL, &plan->cksum_flags);
	mac_ether_offload_info(mp, &meoi);

	if ((meoi.meoi_flags & MEOI_L3INFO_SET) != 0) {
		if (meoi.meoi_l3proto == ETHERTYPE_IP)
			plan->is_ipv4 = B_TRUE;
		else if (meoi.meoi_l3proto == ETHERTYPE_IPV6)
			plan->is_ipv6 = B_TRUE;
	}

	if ((meoi.meoi_flags & MEOI_L4INFO_SET) != 0) {
		if (meoi.meoi_l4proto == IPPROTO_TCP)
			plan->is_tcp = B_TRUE;
		else if (meoi.meoi_l4proto == IPPROTO_UDP)
			plan->is_udp = B_TRUE;
	}

	/*
	 * Keep transport offset for Linux-style patch/padding logic.
	 */
	if ((meoi.meoi_flags & (MEOI_L2INFO_SET | MEOI_L3INFO_SET |
	    MEOI_L4INFO_SET)) ==
	    (MEOI_L2INFO_SET | MEOI_L3INFO_SET | MEOI_L4INFO_SET)) {
		plan->l4_offset = meoi.meoi_l2hlen + meoi.meoi_l3hlen;
	}

	req = plan->cksum_flags & (HCK_FULLCKSUM | HCK_PARTIALCKSUM |
	    HCK_IPV4_HDRCKSUM);

	/*
	 * No checksum request: normal TX packet.
	 */
	if (req == 0) {
		return (B_TRUE);
	}

	/*
	 * First stable stage:
	 * support only IPv4 TX checksum offload.
	 *
	 * For IPv6 or unknown protocol, just reject hw checksum setup.
	 * Caller may still decide to send packet without offload if needed.
	 */
	if (!plan->is_ipv4)
		return (B_FALSE);

	/*
	 * IPv4 header checksum only.
	 */
	if ((plan->cksum_flags & HCK_IPV4_HDRCKSUM) != 0 &&
	    !(plan->is_tcp || plan->is_udp)) {
		plan->opts2_0 |= RE_TX_IPCS_C;
		plan->use_hw_csum = B_TRUE;
		return (B_TRUE);
	}

	/*
	 * Linux-style meaning for IPv4:
	 * TCP => IP + TCP
	 * UDP => IP + UDP
	 */
	if (plan->is_tcp) {
		plan->opts2_0 |= RE_TX_IPCS_C;
		plan->opts2_0 |= RE_TX_TCPCS_C;
		plan->use_hw_csum = B_TRUE;
		return (B_TRUE);
	}

	if (plan->is_udp) {
		plan->opts2_0 |= RE_TX_IPCS_C;
		plan->opts2_0 |= RE_TX_UDPCS_C;
		plan->use_hw_csum = B_TRUE;
		return (B_TRUE);
	}

	/*
	 * Any other IPv4 payload type: don't try hw checksum offload.
	 */
	return (B_FALSE);
}

static int
re_8125_pad(mblk_t **mpp, uint32_t pad_len)
{
	if (pad_len == 0)
		return (DDI_SUCCESS);

	if (re_mblk_append_zeros(mpp, pad_len) != DDI_SUCCESS)
		return (DDI_FAILURE);

	return (DDI_SUCCESS);
}

int
re_tx_encap(re_tx_ring_t *tr, mblk_t *mp)
{
	struct re_softc *sc = tr->sc;
	re_tx_buffer_t *tb;
	re_tx_plan_t plan;
	boolean_t use_copy = B_FALSE;
	size_t pkt_len;
	uint32_t ndescs, first_idx, last_idx, idx;
	uint32_t i;
	uint32_t pad_len;

	re_txeof(tr);

	if (!re_tx_prepare_plan(mp, &plan)) {
		tr->tx_kstat.tx_prepare_fail.value.ui64++;
		tr->tx_kstat.tx_encap_fail.value.ui64++;
		return (DDI_FAILURE);
	}

	pkt_len = re_msg_size(mp);
	if (pkt_len == 0) {
		tr->tx_kstat.tx_encap_fail.value.ui64++;
		return (DDI_FAILURE);
	}

	pad_len = re_get_patch_pad_len(sc, mp, &plan, pkt_len);
	if (pad_len != 0) {
		uint_t emul_flags = 0;
		mblk_t *emul_mp = NULL;
		mac_emul_t emul = 0;

		if (re_8125_pad(&mp, pad_len) != DDI_SUCCESS) {
			tr->tx_kstat.tx_encap_fail.value.ui64++;
			return (DDI_FAILURE);
		}

		pkt_len = re_msg_size(mp);
		if (pkt_len == 0) {
			tr->tx_kstat.tx_encap_fail.value.ui64++;
			return (DDI_FAILURE);
		}

		/*
		 * Linux-style behavior:
		 * if a packet was padded and checksum offload was requested,
		 * emulate checksum in software and do not use hw checksum
		 * for this packet.
		 */
		if (plan.use_hw_csum) {
			emul_flags = plan.cksum_flags;

			if ((plan.cksum_flags &
			    (HCK_FULLCKSUM | HCK_PARTIALCKSUM)) != 0)
				emul |= MAC_HWCKSUM_EMUL;

			if ((plan.cksum_flags & HCK_IPV4_HDRCKSUM) != 0)
				emul |= MAC_IPCKSUM_EMUL;

			if (emul != 0) {
				mac_hw_emul(&mp, &emul_mp, &emul_flags, emul);

				/*
				 * In practice mac_hw_emul may return the new
				 * packet through the second pointer.
				 */
				if (emul_mp != NULL)
					mp = emul_mp;
			}
		}

		/*
		 * Never use hardware checksum on a padded packet.
		 */
		plan.opts1_0 = 0;
		plan.opts2_0 = 0;
		plan.use_hw_csum = B_FALSE;
		plan.cksum_flags = 0;
		plan.pad_applied = B_TRUE;

		pkt_len = re_msg_size(mp);
		if (pkt_len == 0) {
			tr->tx_kstat.tx_encap_fail.value.ui64++;
			return (DDI_FAILURE);
		}
	}

	tb = re_tx_get_free_buf(tr);
	if (tb == NULL) {
		tr->tx_kstat.tx_ring_full.value.ui64++;
		return (DDI_FAILURE);
	}

	if (re_tx_bind_mblk_chain(tb, mp) != DDI_SUCCESS)
		use_copy = B_TRUE;

	if (use_copy) {
		if (re_tx_copy_mblk(tb, mp) != DDI_SUCCESS) {
			tr->tx_kstat.tx_encap_fail.value.ui64++;
			re_tx_buf_abort(tr, tb);
			return (DDI_FAILURE);
		}
		tr->tx_kstat.tx_copy_pkts.value.ui64++;
	} else {
		tr->tx_kstat.tx_bind_pkts.value.ui64++;
	}

	ndescs = tb->tx_ncookies;
	if (ndescs == 0) {
		tr->tx_kstat.tx_encap_fail.value.ui64++;
		re_tx_buf_abort(tr, tb);
		return (DDI_FAILURE);
	}

	mutex_enter(&tr->tx_lock);

	if (!re_tx_desc_avail(tr, ndescs)) {
		tr->tx_kstat.tx_ring_full.value.ui64++;
		mutex_exit(&tr->tx_lock);
		re_tx_buf_abort(tr, tb);
		return (DDI_FAILURE);
	}

	first_idx = tr->cur_tx;
	idx = first_idx;

	for (i = 0; i < ndescs; i++) {
		uint32_t opts1, opts2;
		uint32_t seglen;
		uint64_t daddr;

		seglen = tb->tx_cookies[i].dmac_size;
		daddr = tb->tx_cookies[i].dmac_laddress;

		opts1 = seglen | tr->desc_own_mask;
		opts2 = 0;

		if (i == 0) {
			opts1 |= tr->desc_sof_mask;
			opts1 |= plan.opts1_0;
			opts2 |= plan.opts2_0;
		}

		if (i == ndescs - 1)
			opts1 |= tr->desc_eof_mask;
		if (idx == tr->num_desc - 1)
			opts1 |= tr->desc_eor_mask;

		re_tx_desc_write(tr, idx, daddr, opts1, opts2);
		idx = re_tx_next(tr, idx);
	}

	tb->tx_ndescs = ndescs;
	last_idx = (idx == 0) ? (tr->num_desc - 1) : (idx - 1);
	tb->tx_last_idx = last_idx;

	tr->tx_work_list[first_idx] = tb;

	membar_producer();
	tr->cur_tx = idx;
	re_doorbell(sc, tr);

	tr->tx_kstat.opackets64.value.ui64++;
	tr->tx_kstat.obytes64.value.ui64 += pkt_len;

	if (plan.use_hw_csum) {
		tr->tx_kstat.tx_hw_csum_pkts.value.ui64++;

		if (plan.is_ipv4)
			tr->tx_kstat.tx_ipv4_csum_pkts.value.ui64++;
		if (plan.is_tcp)
			tr->tx_kstat.tx_tcp_csum_pkts.value.ui64++;
		if (plan.is_udp)
			tr->tx_kstat.tx_udp_csum_pkts.value.ui64++;
	}

	tr->tx_last_submit_time = gethrtime();

	mutex_exit(&tr->tx_lock);
	return (DDI_SUCCESS);
}

static void
re_tx_buf_recycle(re_tx_buffer_t *tb)
{
	if (tb->tx_bound && tb->tx_bind_hdl != NULL) {
		(void) ddi_dma_unbind_handle(tb->tx_bind_hdl);
		tb->tx_bound = B_FALSE;
	}

	if (tb->re_tx_mp != NULL) {
		freemsg(tb->re_tx_mp);
		tb->re_tx_mp = NULL;
	}

	tb->tx_ncookies = 0;
	tb->tx_ndescs = 0;
	tb->tx_last_idx = 0;
	tb->tx_copied = B_FALSE;
	bzero(tb->tx_cookies, sizeof (tb->tx_cookies));
}

static void
re_txeof_locked(re_tx_ring_t *tr)
{
	re_tx_buffer_t *tb;
	uint32_t first_idx, last_idx, next_idx;

	ASSERT(mutex_owned(&tr->tx_lock));

	while (tr->dirty_tx != tr->cur_tx) {
		first_idx = tr->dirty_tx;
		tb = tr->tx_work_list[first_idx];

		if (tb == NULL)
			break;

		last_idx = tb->tx_last_idx;

		if (!re_tx_desc_done(tr, last_idx))
			break;

		tr->tx_work_list[first_idx] = NULL;

		re_tx_buf_recycle(tb);
		list_insert_tail(&tr->tx_free_list, tb);
		tr->tx_kstat.tx_reclaim.value.ui64++;
		tr->tx_last_reclaim_time = gethrtime();

		next_idx = re_tx_next(tr, last_idx);
		tr->dirty_tx = next_idx;
	}
}

void
re_txeof(re_tx_ring_t *tr)
{
	mutex_enter(&tr->tx_lock);
	re_txeof_locked(tr);
	mutex_exit(&tr->tx_lock);
}

mblk_t *
re_ring_tx(void *arg, mblk_t *mp)
{
	re_tx_ring_t *tr = arg;
	struct re_softc *sc = tr->sc;
	mblk_t *nmp;

	if (sc->sc_stopped)
		return (mp);

	if (sc->re_link_state != LINK_STATE_UP) {
		freemsgchain(mp);
		return (NULL);
	}

	while (mp != NULL) {
		nmp = mp->b_next;
		mp->b_next = NULL;
		mp->b_prev = NULL;

		if (re_tx_encap(tr, mp) != DDI_SUCCESS) {
			mp->b_next = nmp;
			return (mp);
		}

		mp = nmp;
	}

	return (NULL);
}

/* rings reinit in runtime */
void
re_map_rings_to_dev(struct re_softc *sc)
{
	uint32_t i;

	for (i = 0; i < sc->num_rx_rings; i++)
		re_map_rx_desc_to_dev(sc, &sc->rx_rings[i]);

	for (i = 0; i < sc->num_tx_rings; i++)
		re_map_tx_desc_to_dev(sc, &sc->tx_rings[i]);
}

void
re_free_rings_data(struct re_softc *sc)
{
	re_free_rx_bufs(sc);
	re_free_tx_rings_bufs(sc);
}

int
re_alloc_rings_data(struct re_softc *sc)
{
	if (re_init_rx_rings(sc) != DDI_SUCCESS)
		goto fail;
	if (re_init_tx_rings(sc) != DDI_SUCCESS)
		goto fail;
	return (DDI_SUCCESS);

fail:
	re_free_rings_data(sc);
	return (DDI_FAILURE);
}

static void
re_wake_tx_rings(struct re_softc *sc)
{
	uint32_t i;

	for (i = 0; i < sc->num_tx_rings; i++) {
		re_tx_ring_t *tr = &sc->tx_rings[i];

		if (tr->th != NULL)
			mac_tx_ring_update(sc->mh, tr->th);
	}
}

static int
re_wait_for_intr_tasks(struct re_softc *sc)
{
	if (sc->re_intr_type == DDI_INTR_TYPE_MSIX && sc->intr_vecs != NULL) {
		uint_t i;

		for (i = 0; i < sc->intr_vec_cnt; i++) {
			if (sc->intr_vecs[i].taskq != NULL)
				ddi_taskq_wait(sc->intr_vecs[i].taskq);
		}
	} else {
		if (sc->intr_vec0.taskq != NULL)
			ddi_taskq_wait(sc->intr_vec0.taskq);
	}

	return (0);
}

static int
re_tx_drain(struct re_softc *sc, clock_t ticks)
{
	uint32_t i;

	(void)ticks;

	for (i = 0; i < sc->num_tx_rings; i++)
		re_drain_tx_ring(&sc->tx_rings[i]);

	return (0);
}

int
re_stop_datapath(struct re_softc *sc, clock_t ticks)
{
	int ret;

	ret = re_wait_for_intr_tasks(sc);
	if (ret != 0)
		return (ret);

	ret = re_rx_drain(sc, ticks);
	if (ret != 0)
		return (ret);

	ret = re_tx_drain(sc, ticks);
	if (ret != 0)
		return (ret);

	return (0);
}

void
re_start_datapath(struct re_softc *sc)
{
	/*
	 * Full runtime re-init hardware.
	 * Unside it:
	 * - programming ring base/size/registers
	 * - RX/TX config
	 * - interrupt moderation/masks
	 * - mtu/buffer dependent config
	 * - RSS setup
	 * - packet filter defaults
	 */
	re_init_locked(sc);

	re_set_rx_packet_filter(sc);
	re_wake_tx_rings(sc);
}

boolean_t
re_txeof_should_wake(re_tx_ring_t *tr)
{
	boolean_t wake;

	mutex_enter(&tr->tx_lock);

	re_txeof_locked(tr);

	wake = (re_tx_desc_free(tr) > 0);

	mutex_exit(&tr->tx_lock);

	return (wake);
}
