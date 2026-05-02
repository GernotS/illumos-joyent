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
 * Copyright 2026 Eduard Siboshvili <edsiboshvili@gmail.com>
 * Use is subject to license terms.
 */

/*
 * Interrupt model
 * ---------------
 *
 * Realtek RTL8125/RTL8126/RTL8127 controllers support two interrupt
 * delivery models used by this driver:
 *
 *   1. MSI-X multivector mode
 *   2. MSI single-vector mode
 *
 *
 * MSI-X model
 * -----------
 *
 * MSI-X is the preferred mode.  It allows the driver to route RX, TX and
 * link-change events to separate interrupt vectors and to service rings
 * independently.
 *
 * Realtek MSI-X entries are hardware-defined event slots.  They are not a
 * simple "vector N == queue N" mapping.  RX vectors are usually contiguous,
 * while TX and link-change vectors are placed at chip-specific fixed
 * positions.
 *
 * Example: RTL8125B MSI-X layout
 *
 *        +---------------- RTL8125B MSI-X table ----------------+
 *        |                                                      |
 *        |   [0]   RX queue 0                                   |
 *        |   [1]   RX queue 1                                   |
 *        |   [2]   RX queue 2                                   |
 *        |   [3]   RX queue 3                                   |
 *        |                                                      |
 *        |   [16]  TX queue 0                                   |
 *        |   [18]  TX queue 1                                   |
 *        |                                                      |
 *        |   [21]  Link change / miscellaneous event            |
 *        |                                                      |
 *        +-------------------------------------------------------+
 *
 * Other revisions use different TX and link-change entries.  The mapping
 * is decoded by re_msix_id_to_type() and re_msix_id_to_tx_ring_id().
 *
 * The driver therefore allocates software interrupt-vector objects
 * (re_intr_vector_t) instead of deriving ring ownership directly from the
 * interrupt number.
 *
 *
 * re_intr_vector_t
 * ----------------
 *
 * re_intr_vector_t is the driver's software representation of one interrupt
 * source.  It connects one hardware interrupt entry to the work that must
 * be performed for that entry.
 *
 * In MSI-X mode each active MSI-X entry has one re_intr_vector_t:
 *
 *        +-------------------+
 *        | MSI-X entry N     |
 *        +---------+---------+
 *                  |
 *                  v
 *        +-------------------+
 *        | re_intr_vector_t  |
 *        |                   |
 *        | intr_idx = N      |
 *        | type              |
 *        | rx_ring optional  |
 *        | tx_ring optional  |
 *        | taskq             |
 *        | scheduled flag    |
 *        +---------+---------+
 *                  |
 *                  v
 *        +-------------------+
 *        | deferred task     |
 *        +-------------------+
 *
 * The type field describes what the vector represents:
 *
 *   RE_INTR_VEC_RX      RX-only vector
 *   RE_INTR_VEC_TX      TX-only vector
 *   RE_INTR_VEC_RXTX    combined RX/TX vector
 *   RE_INTR_VEC_LINK    link-change or miscellaneous event vector
 *
 * Each vector owns a small taskq and a scheduled flag.  The hard interrupt
 * handler masks/acknowledges the hardware event and schedules deferred
 * processing.  The deferred task processes RX/TX/link work and then
 * re-enables the hardware interrupt.
 *
 * This keeps the hard interrupt handler small and avoids running GLDv3 MAC
 * callbacks directly from interrupt context.
 *
 *
 * MSI single-vector model
 * -----------------------
 *
 * MSI mode uses one interrupt for all events:
 *
 *        +-------------------------------+
 *        | MSI vector 0                  |
 *        |                               |
 *        | RX + TX + link + errors       |
 *        +---------------+---------------+
 *                        |
 *                        v
 *        +-------------------------------+
 *        | sc->intr_vec0                 |
 *        |                               |
 *        | rx_ring = ring 0              |
 *        | tx_ring = ring 0              |
 *        | pending_status                |
 *        | single taskq                  |
 *        +---------------+---------------+
 *                        |
 *                        v
 *        +-------------------------------+
 *        | re_intr_task_1q()             |
 *        +-------------------------------+
 *
 * In this mode the interrupt handler reads the device ISR, acknowledges the
 * relevant bits, records them in intr_vec0.pending_status and schedules a
 * single deferred task.  The deferred task handles RX, TX, link-change and
 * error processing for the single-queue fallback path.
 *
 *
 * Timer interrupt mode
 * --------------------
 *
 * Timer interrupt mode is built on top of the MSI/single-vector path.
 * It uses the legacy interrupt generation logic and periodically delivers
 * completion events through the same re_intr_1q() / re_intr_task_1q()
 * path.  This mode is useful as a compatibility/stability fallback, but it
 * has different packet-rate characteristics from MSI-X multiqueue mode.
 *
 *
 * Locking and taskq rules
 * -----------------------
 *
 * Interrupt tasks may acquire the driver mutex and ring locks.  Therefore
 * code that waits for interrupt taskqs to drain must not hold the driver
 * mutex.  In particular, stop/shutdown paths must disable new interrupt
 * scheduling first, drop the driver mutex, wait for taskq completion, and
 * only then continue resource teardown.
 */

#include "re.h"

static void re_intr_task_1q(void *arg);

static int
re_intr_update_pri_cap(struct re_softc *sc)
{
	uint_t pri = 0;
	int cap = 0;
	int rc;

	sc->intr_pri = 0;
	sc->re_intr_cap = 0;

	if (sc->htable == NULL || sc->intr_cnt == 0 || sc->htable[0] == NULL)
		return (DDI_FAILURE);

	rc = ddi_intr_get_pri(sc->htable[0], &pri);
	if (rc != DDI_SUCCESS)
		return (DDI_FAILURE);

	rc = ddi_intr_get_cap(sc->htable[0], &cap);
	if (rc != DDI_SUCCESS)
		return (DDI_FAILURE);

	sc->intr_pri = pri;
	sc->re_intr_cap = cap;
	return (DDI_SUCCESS);
}

int
re_intr_enable_all(struct re_softc *sc)
{
	if ((sc->re_intr_cap & DDI_INTR_FLAG_BLOCK) != 0)
		return (ddi_intr_block_enable(sc->htable, sc->intr_cnt));

	for (uint16_t i = 0; i < sc->intr_cnt; i++) {
		if (ddi_intr_enable(sc->htable[i]) != DDI_SUCCESS)
			return (DDI_FAILURE);
	}
	return (DDI_SUCCESS);
}

void
re_intr_disable_all(struct re_softc *sc)
{
	if ((sc->re_intr_cap & DDI_INTR_FLAG_BLOCK) != 0) {
		(void)ddi_intr_block_disable(sc->htable, sc->intr_cnt);
		return;
	}

	for (uint16_t i = 0; i < sc->intr_cnt; i++)
		(void)ddi_intr_disable(sc->htable[i]);
}

static void
re_intr_free_handles(struct re_softc *sc)
{
	if (sc->htable == NULL)
		return;

	for (uint16_t i = 0; i < sc->intr_cnt; i++) {
		if (sc->htable[i] != NULL) {
			(void)ddi_intr_free(sc->htable[i]);
			sc->htable[i] = NULL;
		}
	}
	sc->intr_cnt = 0;
	sc->intr_pri = 0;
	sc->re_intr_cap = 0;
}

static void
re_intr_free_table(struct re_softc *sc)
{
	if (sc->htable != NULL) {
		size_t sz = (size_t)sc->intr_cnt_max * sizeof (ddi_intr_handle_t);
		kmem_free(sc->htable, sz);
		sc->htable = NULL;
	}
	sc->intr_cnt_max = 0;
}

static int
re_intr_try_msix(struct re_softc *sc, uint16_t desired)
{
	int count = 0, actual = 0, err;
	size_t sz;

	/* discover max */
	err = ddi_intr_get_nintrs(sc->dev, DDI_INTR_TYPE_MSIX, &count);
	if (err != DDI_SUCCESS || count <= 0)
		return (DDI_FAILURE);

	if (desired == 0)
		return (DDI_FAILURE);

	if (desired > (uint16_t)count)
		return (DDI_FAILURE);

	/* allocate table sized to max (so we can keep consistent; optional) */
	sc->intr_cnt_max = (uint16_t)count;
	sz = (size_t)sc->intr_cnt_max * sizeof (ddi_intr_handle_t);
	sc->htable = kmem_zalloc(sz, KM_SLEEP);

	err = ddi_intr_alloc(sc->dev, sc->htable, DDI_INTR_TYPE_MSIX,
	    0, desired, &actual, DDI_INTR_ALLOC_NORMAL);

	if (err != DDI_SUCCESS || actual != desired) {
		/* rollback */
		if (err == DDI_SUCCESS && actual > 0) {
			sc->intr_cnt = (uint16_t)actual;
			re_intr_free_handles(sc);
		}
		re_intr_free_table(sc);
		return (DDI_FAILURE);
	}

	sc->re_intr_type = DDI_INTR_TYPE_MSIX;
	sc->intr_cnt = (uint16_t)actual;

	/* pri/cap must succeed for our “clean” model */
	if (re_intr_update_pri_cap(sc) != DDI_SUCCESS) {
		re_intr_free_handles(sc);
		re_intr_free_table(sc);
		sc->re_intr_type = 0;
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

static int
re_intr_try_msi(struct re_softc *sc)
{
	int count = 0, actual = 0, err;
	size_t sz;
	uint16_t req = 1;

	err = ddi_intr_get_nintrs(sc->dev, DDI_INTR_TYPE_MSI, &count);
	if (err != DDI_SUCCESS || count <= 0)
		return (DDI_FAILURE);

	sc->intr_cnt_max = (uint16_t)count;
	sz = (size_t)sc->intr_cnt_max * sizeof (ddi_intr_handle_t);
	sc->htable = kmem_zalloc(sz, KM_SLEEP);

	err = ddi_intr_alloc(sc->dev, sc->htable, DDI_INTR_TYPE_MSI,
	    0, req, &actual, DDI_INTR_ALLOC_NORMAL);

	if (err != DDI_SUCCESS || actual != 1) {
		if (err == DDI_SUCCESS && actual > 0) {
			sc->intr_cnt = (uint16_t)actual;
			re_intr_free_handles(sc);
		}
		re_intr_free_table(sc);
		return (DDI_FAILURE);
	}

	sc->re_intr_type = DDI_INTR_TYPE_MSI;
	sc->intr_cnt = (uint16_t)actual;

	if (re_intr_update_pri_cap(sc) != DDI_SUCCESS) {
		re_intr_free_handles(sc);
		re_intr_free_table(sc);
		sc->re_intr_type = 0;
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

int
re_alloc_intr(struct re_softc *sc)
{
	int intr_types = 0;

	sc->re_intr_type = 0;
	sc->htable = NULL;
	sc->intr_cnt = 0;
	sc->intr_cnt_max = 0;
	sc->re_intr_cap = 0;
	sc->intr_pri = 0;

	if (ddi_intr_get_supported_types(sc->dev, &intr_types) != DDI_SUCCESS) {
		return (DDI_FAILURE);
	}

	if ((intr_types & DDI_INTR_TYPE_MSIX) && !sc->force_msi) {
		uint16_t desired = sc->msix_min_reqired;

		if (desired > 0 &&
		    re_intr_try_msix(sc, desired) == DDI_SUCCESS)
			return (DDI_SUCCESS);

		/*
		 * Fallback to single-vector MSI-X.
		 * It will work in 1q path.
		 */
		if (re_intr_try_msix(sc, 1) == DDI_SUCCESS)
			return (DDI_SUCCESS);
	}

	if (intr_types & DDI_INTR_TYPE_MSI) {
		if (re_intr_try_msi(sc) == DDI_SUCCESS)
			return (DDI_SUCCESS);
	}

	return (DDI_FAILURE);
}

void
re_free_intr(struct re_softc *sc)
{
	/* call after stop path: hw masked, disabled, handlers removed */
	re_intr_free_handles(sc);
	re_intr_free_table(sc);
	sc->re_intr_type = 0;
}

static void
re_msix_free_tail_locked(struct re_softc *sc, int to_free)
{
	while (to_free-- > 0) {
		int idx = (int)sc->intr_allocated - 1;
		if (idx < 0)
			break;

		/* never free below intr_used */
		if (sc->intr_allocated <= sc->intr_used)
			break;

		if (sc->htable[idx] != NULL) {
			(void) ddi_intr_free(sc->htable[idx]);
			sc->htable[idx] = NULL;
		}
		sc->intr_allocated--;
	}
}

static int
re_irm_cb(dev_info_t *dip, ddi_cb_action_t action,
    void *cbarg, void *arg1, void *arg2)
{
	struct re_softc *sc = arg1;
	int delta = (int)(uintptr_t)cbarg;

	(void)dip;
	(void)arg2;

	if ((action == DDI_CB_INTR_ADD &&
	    sc->intr_allocated + delta > sc->intr_cnt_max) ||
	    (action == DDI_CB_INTR_REMOVE &&
	    (int)sc->intr_allocated - delta < (int)sc->msix_min_reqired))
		return (DDI_FAILURE);

	if ((sc->attach_state & RE_ATTACH_MAC_START) == 0)
		return (DDI_FAILURE);

	mutex_enter(&sc->mtx);

	switch (action) {
	case DDI_CB_INTR_ADD: {
		uint32_t new_avail = (uint32_t)sc->intr_avail + (uint32_t)delta;

		if (new_avail > sc->intr_cnt_max)
			new_avail = sc->intr_cnt_max;

		sc->intr_avail = (uint16_t)new_avail;

		if (sc->intr_allocated < sc->intr_used) {
			mutex_exit(&sc->mtx);
			return (DDI_FAILURE);
		}

		if (sc->intr_allocated < sc->intr_used)
			sc->intr_allocated = sc->intr_used;

		break;
	}

	case DDI_CB_INTR_REMOVE: {
		uint32_t old_avail = sc->intr_avail;
		uint32_t new_avail = (delta >= (int)old_avail) ?
		    0 : (old_avail - (uint32_t)delta);
		int must_free;
		int spare;
		int can_free_now;

		sc->intr_avail = (uint16_t)new_avail;

		must_free = (int)sc->intr_allocated - (int)new_avail;
		if (must_free < 0)
			must_free = 0;

		spare = (int)sc->intr_allocated - (int)sc->intr_used;
		if (spare < 0)
			spare = 0;

		can_free_now = (must_free <= spare) ? must_free : spare;
		if (can_free_now > 0)
			re_msix_free_tail_locked(sc, can_free_now);

		if ((int)sc->intr_allocated > (int)new_avail)
			sc->irm_pending = 1;

		break;
	}

	default:
		mutex_exit(&sc->mtx);
		return (DDI_ENOTSUP);
	}

	mutex_exit(&sc->mtx);
	return (DDI_SUCCESS);
}

int
re_irm_register(struct re_softc *sc)
{
	int rc;

	if (sc->irm_registered)
		return (DDI_SUCCESS);

	rc = ddi_cb_register(sc->dev, DDI_CB_FLAG_INTR, re_irm_cb,
	    sc, NULL, &sc->cb_hdl);
	if (rc != DDI_SUCCESS) {
		sc->cb_hdl = NULL;
		return (DDI_FAILURE);
	}

	sc->irm_registered = 1;
	return (DDI_SUCCESS);
}

void
re_irm_unregister(struct re_softc *sc)
{
	if (!sc->irm_registered)
		return;

	(void) ddi_cb_unregister(sc->cb_hdl);
	sc->cb_hdl = NULL;
	sc->irm_registered = 0;
}

static int
re_intr_vector_init_1q(struct re_softc *sc)
{
	re_intr_vector_t *iv;
	char tq_name[32];

	iv = &sc->intr_vec0;
	bzero(iv, sizeof (*iv));

	iv->sc = sc;
	iv->rx_ring = &sc->rx_rings[0];
	iv->tx_ring = &sc->tx_rings[0];
	iv->intr_idx = 0;
	iv->scheduled = B_FALSE;
	iv->rx_ring->intr_idx = 0;
	iv->tx_ring->intr_idx = 0;
	mutex_init(&iv->lock, NULL, MUTEX_DRIVER, DDI_INTR_PRI(sc->intr_pri));

	(void)snprintf(tq_name, sizeof (tq_name),
			"%s%d-msi",
			RE_MOD_NAME,
			ddi_get_instance(sc->dev));
	iv->taskq = ddi_taskq_create(sc->dev, tq_name,
	    1, TASKQ_DEFAULTPRI, 0);
	if (iv->taskq == NULL) {
		mutex_destroy(&iv->lock);
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

static void
re_intr_vector_fini_1q(struct re_softc *sc)
{
	re_intr_vector_t *iv;

	if (sc == NULL)
		return;

	iv = &sc->intr_vec0;

	if (iv->taskq != NULL) {
		ddi_taskq_destroy(iv->taskq);
		iv->taskq = NULL;
	}

	mutex_destroy(&iv->lock);

	iv->sc = NULL;
	iv->rx_ring = NULL;
	iv->tx_ring = NULL;
	iv->scheduled = B_FALSE;
}

static void
re_intr_task_1q(void *arg)
{
	re_intr_vector_t *iv = arg;
	struct re_softc *sc = iv->sc;
	re_rx_ring_t *rr = iv->rx_ring;
	re_tx_ring_t *tr = iv->tx_ring;
	uint32_t status;
	mblk_t *mp = NULL;
	boolean_t tx_wake = B_FALSE;
	boolean_t do_link = B_FALSE;
	boolean_t do_work = B_FALSE;

	mutex_enter(&iv->lock);
	status = iv->pending_status;
	iv->pending_status = 0;
	mutex_exit(&iv->lock);

	RE_LOCK(sc);

	if (sc->suspended ||
	    sc->reset_pending ||
	    sc->reset_running ||
	    ((sc->attach_state & RE_ATTACH_MAC_START) == 0)) {
		goto out;
	}

	if ((status & sc->intr_mask_err) != 0) {
		re_schedule_reset_locked(sc, RE_RESET_INTR_ERR);
		goto out;
	}

	if ((status & sc->intr_mask_link) != 0) {
		do_link = B_TRUE;
	}

	if ((status & (sc->intr_mask | sc->timer_intr_mask)) != 0)
		do_work = B_TRUE;

	if (do_work) {
		if (rr != NULL) {
			mutex_enter(&rr->rx_lock);
			mp = re_ring_rx(rr, 0);
			mutex_exit(&rr->rx_lock);
		}

		if (tr != NULL) {
			if (tr->th != NULL)
				tx_wake = re_txeof_should_wake(tr);
			else
				re_txeof(tr);
		}
	}

out:
	mutex_enter(&iv->lock);
	iv->scheduled = B_FALSE;
	mutex_exit(&iv->lock);

	re_enable_imr_8125(sc);
	RE_UNLOCK(sc);

	if (do_link)
		re_check_link_status(sc);

	if (tr != NULL && tr->th != NULL && tx_wake)
		mac_tx_ring_update(sc->mh, tr->th);

	if (mp != NULL && rr != NULL)
		mac_rx_ring(sc->mh, rr->rh, mp, rr->ring_gen);
}

static uint_t
re_intr_1q(caddr_t arg1, caddr_t arg2)
{
	re_intr_vector_t *iv = (re_intr_vector_t *)arg1;
	struct re_softc *sc = iv->sc;
	uint32_t raw, ack, status, mask;

	(void)arg2;
	RE_LOCK(sc);

	if (sc->suspended ||
	    ((sc->attach_state & RE_ATTACH_MAC_START) == 0)) {
		RE_UNLOCK(sc);
		return (DDI_INTR_UNCLAIMED);
	}

	re_disable_imr_8125(sc);

	raw = re_get_isr_8125(sc);

	if (raw == 0 || raw == 0xffffffff) {
		RE_UNLOCK(sc);
		return (DDI_INTR_UNCLAIMED);
	}

	/*
	 * Work mask: normal interrupt causes plus timer interrupt causes.
	 */
	mask = sc->intr_mask | sc->timer_intr_mask;
	status = raw & mask;

	/*
	 * Ack only bits that belong to our interrupt model.
	 * Do not blindly ack all ISR bits.
	 */
	ack = status;

	if (!sc->use_new_intr_mapping)
		ack &= ~RE_ISR_RX_FIFO_OFLOW;

	if (ack != 0)
		re_set_isr_8125(sc, ack);

	if (status == 0) {
		RE_UNLOCK(sc);
		return (DDI_INTR_UNCLAIMED);
	}

	mutex_enter(&iv->lock);
	iv->pending_status |= status;

	if (!iv->scheduled) {
		iv->scheduled = B_TRUE;

		if (ddi_taskq_dispatch(iv->taskq, re_intr_task_1q, iv,
		    DDI_NOSLEEP) == DDI_FAILURE) {
			iv->scheduled = B_FALSE;
			iv->pending_status &= ~status;
			mutex_exit(&iv->lock);
			re_enable_imr_8125(sc);
			RE_UNLOCK(sc);
			return (DDI_INTR_UNCLAIMED);
		}
	}

	mutex_exit(&iv->lock);

	RE_UNLOCK(sc);
	return (DDI_INTR_CLAIMED);
}

/*
 * MSI-X per-vector mask/unmask/ack.
 */
static void
re_msix_hw_intr_disable(struct re_softc *sc, uint32_t intr_id)
{
	re_disable_hw_layered_interrupt(sc, intr_id);
}

static void
re_msix_hw_intr_enable(struct re_softc *sc, uint32_t intr_id)
{
	re_enable_hw_layered_interrupt(sc, intr_id);
}

static boolean_t
re_intr_task_schedule_msix(re_intr_vector_t *iv, void (*func)(void *))
{
	boolean_t ok = B_FALSE;

	mutex_enter(&iv->lock);

	if (!iv->scheduled) {
		iv->scheduled = B_TRUE;
		if (ddi_taskq_dispatch(iv->taskq, func, iv,
		    DDI_NOSLEEP) != DDI_FAILURE) {
			ok = B_TRUE;
		} else {
			iv->scheduled = B_FALSE;
		}
	}

	mutex_exit(&iv->lock);

	return (ok);
}

static void
re_intr_task_done_msix(re_intr_vector_t *iv)
{
	mutex_enter(&iv->lock);
	iv->scheduled = B_FALSE;
	mutex_exit(&iv->lock);
}

static void
re_intr_msix_rx_task(void *arg)
{
	re_intr_vector_t *iv = arg;
	struct re_softc *sc = iv->sc;
	re_rx_ring_t *rr = iv->rx_ring;
	mblk_t *mp = NULL;

	if (sc->suspended || ((sc->attach_state & RE_ATTACH_MAC_START) == 0))
		goto out_enable;

	mutex_enter(&rr->rx_lock);
	mp = re_ring_rx(rr, 0);
	mutex_exit(&rr->rx_lock);

	if (mp != NULL)
		mac_rx_ring(sc->mh, rr->rh, mp, rr->ring_gen);

out_enable:
	re_intr_task_done_msix(iv);
	re_msix_hw_intr_enable(sc, iv->intr_idx);
}

static void
re_intr_msix_tx_task(void *arg)
{
	re_intr_vector_t *iv = arg;
	struct re_softc *sc = iv->sc;
	re_tx_ring_t *tr = iv->tx_ring;
	boolean_t wake = B_FALSE;

	if (sc->suspended || ((sc->attach_state & RE_ATTACH_MAC_START) == 0))
		goto out_enable;

	wake = re_txeof_should_wake(tr);

	if (tr->th != NULL && wake)
		mac_tx_ring_update(sc->mh, tr->th);

out_enable:
	re_intr_task_done_msix(iv);
	re_msix_hw_intr_enable(sc, iv->intr_idx);
}

static void
re_intr_msix_rxtx_task(void *arg)
{
	re_intr_vector_t *iv = arg;
	struct re_softc *sc = iv->sc;
	re_rx_ring_t *rr = iv->rx_ring;
	re_tx_ring_t *tr = iv->tx_ring;
	mblk_t *mp = NULL;
	boolean_t wake = B_FALSE;

	if (sc->suspended || ((sc->attach_state & RE_ATTACH_MAC_START) == 0))
		goto out_enable;

	wake = re_txeof_should_wake(tr);

	if (tr->th != NULL && wake)
		mac_tx_ring_update(sc->mh, tr->th);

	mutex_enter(&rr->rx_lock);
	mp = re_ring_rx(rr, 0);
	mutex_exit(&rr->rx_lock);

	if (mp != NULL)
		mac_rx_ring(sc->mh, rr->rh, mp, rr->ring_gen);

out_enable:
	re_intr_task_done_msix(iv);
	re_msix_hw_intr_enable(sc, iv->intr_idx);
}

static void
re_intr_msix_linkchg_task(void *arg)
{
	re_intr_vector_t *iv = arg;
	struct re_softc *sc = iv->sc;

	if (!sc->suspended &&
		((sc->attach_state & RE_ATTACH_MAC_START) != 0))
		re_check_link_status(sc);

	re_intr_task_done_msix(iv);
	re_msix_hw_intr_enable(sc, iv->intr_idx);
}

static uint_t
re_intr_msix_common(re_intr_vector_t *iv, void (*task)(void *))
{
	struct re_softc *sc = iv->sc;

	re_msix_hw_intr_disable(sc, iv->intr_idx);
	re_clear_hw_isr_v2(sc, iv->intr_idx);

	if (!re_intr_task_schedule_msix(iv, task)) {
		re_msix_hw_intr_enable(sc, iv->intr_idx);
		return (DDI_INTR_UNCLAIMED);
	}

	return (DDI_INTR_CLAIMED);
}

static uint_t
re_intr_msix_tx(caddr_t arg1, caddr_t arg2)
{
	re_intr_vector_t *iv = (re_intr_vector_t *)arg1;
	(void)arg2;

	return (re_intr_msix_common(iv, re_intr_msix_tx_task));
}

static uint_t
re_intr_msix_rx(caddr_t arg1, caddr_t arg2)
{
	re_intr_vector_t *iv = (re_intr_vector_t *)arg1;
	(void)arg2;

	return (re_intr_msix_common(iv, re_intr_msix_rx_task));
}

static uint_t
re_intr_msix_rxtx(caddr_t arg1, caddr_t arg2)
{
	re_intr_vector_t *iv = (re_intr_vector_t *)arg1;
	(void)arg2;

	return (re_intr_msix_common(iv, re_intr_msix_rxtx_task));
}

static uint_t
re_intr_msix_linkchg(caddr_t arg1, caddr_t arg2)
{
	re_intr_vector_t *iv = (re_intr_vector_t *)arg1;
	(void)arg2;

	return (re_intr_msix_common(iv, re_intr_msix_linkchg_task));
}

static re_intr_vec_type_t
re_msix_id_to_type(struct re_softc *sc, int intr_id)
{
	re_intr_vec_type_t type = RE_INTR_VEC_LINK;

	if (intr_id < sc->num_rx_rings) {
		if ((sc->HwSuppIsrVer == 3 || sc->HwSuppIsrVer == 4) &&
		    intr_id < sc->num_tx_rings)
			type = RE_INTR_VEC_RXTX;
		else
			type = RE_INTR_VEC_RX;
	}

	switch (sc->HwSuppIsrVer) {
	case 2:
		if (intr_id == 16 || (intr_id == 18 && sc->num_tx_rings > 1))
			type = RE_INTR_VEC_TX;
		if (intr_id == 21)
			type = RE_INTR_VEC_LINK;
		break;
	case 3:
		if (intr_id == 21)
			type = RE_INTR_VEC_LINK;
		break;
	case 4:
		if (intr_id == 29)
			type = RE_INTR_VEC_LINK;
		break;
	case 5:
		if (intr_id == 16 || (intr_id == 17 && sc->num_tx_rings > 1))
			type = RE_INTR_VEC_TX;
		if (intr_id == 18)
			type = RE_INTR_VEC_LINK;
		break;
	case 6:
		if (intr_id == 8 || (intr_id == 9 && sc->num_tx_rings > 1))
			type = RE_INTR_VEC_TX;
		if (intr_id == 29)
			type = RE_INTR_VEC_LINK;
		break;
	case 7:
		if (intr_id == 27 || (intr_id == 28 && sc->num_tx_rings > 1))
			type = RE_INTR_VEC_TX;
		if (intr_id == 29)
			type = RE_INTR_VEC_LINK;
		break;
	default:
		break;
	}

	return (type);
}

int
re_msix_id_to_tx_ring_id(struct re_softc *sc, int intr_id)
{
	int ret = -1;

	switch (sc->HwSuppIsrVer) {
	case 2:
		if (intr_id == 16)
			ret = 0;
		if (intr_id == 18)
			ret = 1;
		break;
	case 3:
	case 4:
		ret = intr_id;
		break;
	case 5:
		if (intr_id == 16)
			ret = 0;
		if (intr_id == 17)
			ret = 1;
		break;
	case 6:
		if (intr_id == 8)
			ret = 0;
		if (intr_id == 9)
			ret = 1;
		break;
	case 7:
		if (intr_id == 27)
			ret = 0;
		if (intr_id == 28)
			ret = 1;
		break;
	default:
		break;
	}

	return (ret);
}

static int
re_intr_vector_init_msix(struct re_softc *sc)
{
	uint_t i;

	sc->intr_vecs = kmem_zalloc(sizeof (re_intr_vector_t) * sc->intr_cnt,
	    KM_SLEEP);
	sc->intr_vec_cnt = sc->intr_cnt;

	for (i = 0; i < sc->intr_vec_cnt; i++) {
		re_intr_vector_t *iv = &sc->intr_vecs[i];
		int tx_ring_id;
		char tq_name[32];
		bzero(iv, sizeof (*iv));

		iv->sc = sc;
		iv->intr_idx = i;
		iv->type = re_msix_id_to_type(sc, i);
		iv->scheduled = B_FALSE;

		switch (iv->type) {
		case RE_INTR_VEC_RX:
			if (i < sc->num_rx_rings) {
				iv->rx_ring = &sc->rx_rings[i];
				iv->rx_ring->intr_handle = sc->htable[i];
				iv->rx_ring->intr_idx = i;
			}
			break;

		case RE_INTR_VEC_TX:
			tx_ring_id = re_msix_id_to_tx_ring_id(sc, i);
			if (tx_ring_id >= 0 && tx_ring_id < sc->num_tx_rings) {
				iv->tx_ring = &sc->tx_rings[tx_ring_id];
				iv->tx_ring->intr_handle = sc->htable[i];
			}
			break;

		case RE_INTR_VEC_RXTX:
			if (i < sc->num_rx_rings) {
				iv->rx_ring = &sc->rx_rings[i];
				iv->rx_ring->intr_handle = sc->htable[i];
				iv->rx_ring->intr_idx = i;
			}

			tx_ring_id = re_msix_id_to_tx_ring_id(sc, i);
			if (tx_ring_id >= 0 && tx_ring_id < sc->num_tx_rings) {
				iv->tx_ring = &sc->tx_rings[tx_ring_id];
				iv->tx_ring->intr_handle = sc->htable[i];
				iv->tx_ring->intr_idx = i;
			}
			break;

		case RE_INTR_VEC_LINK:
		default:
			break;
		}

		mutex_init(&iv->lock, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(sc->intr_pri));

		(void)snprintf(tq_name, sizeof (tq_name),
			"%s%d-msix-%u",
			RE_MOD_NAME,
			ddi_get_instance(sc->dev),
			i);
		iv->taskq = ddi_taskq_create(sc->dev, tq_name,
		    1, TASKQ_DEFAULTPRI, 0);
		if (iv->taskq == NULL)
			goto fail;
	}

	return (DDI_SUCCESS);

fail:
	while (i-- != 0) {
		re_intr_vector_t *iv = &sc->intr_vecs[i];
		if (iv->taskq != NULL)
			ddi_taskq_destroy(iv->taskq);
		mutex_destroy(&iv->lock);
	}

	kmem_free(sc->intr_vecs,
	    sizeof (re_intr_vector_t) * sc->intr_vec_cnt);
	sc->intr_vecs = NULL;
	sc->intr_vec_cnt = 0;

	return (DDI_FAILURE);
}

static void
re_intr_vector_fini_msix(struct re_softc *sc)
{
	uint_t i;

	if (sc == NULL || sc->intr_vecs == NULL)
		return;

	for (i = 0; i < sc->intr_vec_cnt; i++) {
		re_intr_vector_t *iv = &sc->intr_vecs[i];

		if (iv->taskq != NULL) {
			ddi_taskq_destroy(iv->taskq);
			iv->taskq = NULL;
		}

		mutex_destroy(&iv->lock);
	}

	kmem_free(sc->intr_vecs,
	    sizeof (re_intr_vector_t) * sc->intr_vec_cnt);

	sc->intr_vecs = NULL;
	sc->intr_vec_cnt = 0;
}

static uint_t
(*re_intr_handler_for_type(re_intr_vec_type_t type))(caddr_t, caddr_t)
{
	switch (type) {
	case RE_INTR_VEC_RX:
		return (re_intr_msix_rx);
	case RE_INTR_VEC_TX:
		return (re_intr_msix_tx);
	case RE_INTR_VEC_LINK:
		return (re_intr_msix_linkchg);
	case RE_INTR_VEC_RXTX:
		return (re_intr_msix_rxtx);
	default:
		return (NULL);
	}
}

static int
re_intr_add_handler(struct re_softc *sc, int intr_id)
{
	re_intr_vector_t *iv;
	uint_t (*handler)(caddr_t, caddr_t);

	iv = &sc->intr_vecs[intr_id];

	handler = re_intr_handler_for_type(iv->type);
	if (handler == NULL)
		return (DDI_FAILURE);

	return (ddi_intr_add_handler(sc->htable[intr_id],
	    handler, (caddr_t)iv, NULL));
}

static int
re_intr_remove_handler(struct re_softc *sc)
{
	uint_t i;
	int ret = DDI_SUCCESS;

	if (sc == NULL || sc->htable == NULL)
		return (DDI_SUCCESS);

	for (i = 0; i < sc->intr_cnt; i++) {
		if (sc->htable[i] == NULL)
			continue;
		if (ddi_intr_remove_handler(sc->htable[i]) != DDI_SUCCESS)
			ret = DDI_FAILURE;
	}

	return (ret);
}

int
re_intr_setup_handlers(struct re_softc *sc)
{
	uint_t i;

	if (sc->re_intr_type == DDI_INTR_TYPE_MSIX && sc->intr_cnt > 1) {
		if (re_intr_vector_init_msix(sc) != DDI_SUCCESS)
			return (DDI_FAILURE);

		for (i = 0; i < sc->intr_cnt; i++) {
			if (re_intr_add_handler(sc, i) != DDI_SUCCESS)
				goto fail_msix;
		}
		return (DDI_SUCCESS);
	}

	if (re_intr_vector_init_1q(sc) != DDI_SUCCESS)
		return (DDI_FAILURE);

	if (ddi_intr_add_handler(sc->htable[0], re_intr_1q,
	    (caddr_t)&sc->intr_vec0, NULL) != DDI_SUCCESS) {
		re_intr_vector_fini_1q(sc);
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);

fail_msix:
	while (i-- != 0)
		(void) ddi_intr_remove_handler(sc->htable[i]);

	re_intr_vector_fini_msix(sc);
	return (DDI_FAILURE);
}

void
re_intr_teardown_handlers(struct re_softc *sc)
{
	if (sc == NULL || sc->htable == NULL)
		return;

	if (sc->re_intr_type == DDI_INTR_TYPE_MSIX && sc->intr_cnt > 1) {
		(void) re_intr_remove_handler(sc);
		re_intr_vector_fini_msix(sc);
	} else {
		(void) ddi_intr_remove_handler(sc->htable[0]);
		re_intr_vector_fini_1q(sc);
	}
}
