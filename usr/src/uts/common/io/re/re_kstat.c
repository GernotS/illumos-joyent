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

#include "re.h"

boolean_t
re_rx_ring_kstats_init(struct re_softc *sc, re_rx_ring_t *rr)
{
	char name[KSTAT_STRLEN];
	kstat_t *ksp;
	re_rx_ring_kstats_t *ks;

	(void)snprintf(name, sizeof (name), "rx_ring%u", rr->index);

	ksp = kstat_create(RE_MOD_NAME, ddi_get_instance(sc->dev),
	    name, "net", KSTAT_TYPE_NAMED,
	    sizeof (re_rx_ring_kstats_t) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (ksp == NULL)
		return (B_FALSE);

	rr->rx_ksp = ksp;
	ks = &rr->rx_kstat;
	ksp->ks_data = ks;

	kstat_named_init(&ks->ipackets64, "ipackets64", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rbytes64,   "rbytes64",   KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_desc_err,    "rx_desc_err",    KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_allocb_fail, "rx_allocb_fail", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_bind_fail,   "rx_bind_fail",   KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_copy_pkts,   "rx_copy_pkts",   KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_bind_pkts,   "rx_bind_pkts",   KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_recycled,    "rx_recycled",    KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_no_sof_no_eof, "rx_no_sof_no_eof", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_restart_assembly, "rx_restart_assembly", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_fragment_pkts, "rx_fragment_pkts", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_single_desc_pkts, "rx_single_desc_pkts", KSTAT_DATA_UINT64);
	kstat_install(ksp);
	return (B_TRUE);
}

void
re_rx_ring_stats_fini(struct re_softc *sc, re_rx_ring_t *rr)
{
	(void) sc;

	if (rr->rx_ksp != NULL) {
		kstat_delete(rr->rx_ksp);
		rr->rx_ksp = NULL;
	}
}

boolean_t
re_tx_ring_kstat_init(struct re_softc *sc, re_tx_ring_t *tr)
{
	char name[KSTAT_STRLEN];
	kstat_t *ksp;
	re_tx_ring_kstats_t *ks;

	(void)snprintf(name, sizeof (name), "tx_ring%u", tr->index);

	ksp = kstat_create(RE_MOD_NAME, ddi_get_instance(sc->dev),
	    name, "net", KSTAT_TYPE_NAMED,
	    sizeof (re_tx_ring_kstats_t) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (ksp == NULL)
		return (B_FALSE);

	tr->tx_ksp = ksp;
	ks = &tr->tx_kstat;
	ksp->ks_data = ks;

	kstat_named_init(&ks->opackets64,  "opackets64",  KSTAT_DATA_UINT64);
	kstat_named_init(&ks->obytes64,    "obytes64",    KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_reclaim,  "tx_reclaim",  KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_encap_fail, "tx_encap_fail", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_bind_fail,  "tx_bind_fail",  KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_copy_pkts,  "tx_copy_pkts",  KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_bind_pkts,  "tx_bind_pkts",  KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_ring_full,  "tx_ring_full",  KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_hw_csum_pkts,
    "tx_hw_csum_pkts", KSTAT_DATA_UINT64);

	kstat_named_init(&ks->tx_lso_pkts,
		"tx_lso_pkts", KSTAT_DATA_UINT64);

	kstat_named_init(&ks->tx_lso_bytes,
		"tx_lso_bytes", KSTAT_DATA_UINT64);

	kstat_named_init(&ks->tx_lso_patch_pkts,
		"tx_lso_patch_pkts", KSTAT_DATA_UINT64);

	kstat_named_init(&ks->tx_ipv4_csum_pkts,
		"tx_ipv4_csum_pkts", KSTAT_DATA_UINT64);

	kstat_named_init(&ks->tx_tcp_csum_pkts,
		"tx_tcp_csum_pkts", KSTAT_DATA_UINT64);

	kstat_named_init(&ks->tx_udp_csum_pkts,
		"tx_udp_csum_pkts", KSTAT_DATA_UINT64);

	kstat_named_init(&ks->tx_prepare_fail,
		"tx_prepare_fail", KSTAT_DATA_UINT64);
	kstat_install(ksp);
	return (B_TRUE);
}

void
re_tx_ring_kstat_fini(struct re_softc *sc, re_tx_ring_t *tr)
{
	(void) sc;

	if (tr->tx_ksp != NULL) {
		kstat_delete(tr->tx_ksp);
		tr->tx_ksp = NULL;
	}
}

static int
re_softc_kstat_update(kstat_t *ksp, int rw)
{
	struct re_softc *sc = ksp->ks_private;
	re_softc_kstats_t *ks;
	struct re_stats *st;

	if (rw == KSTAT_WRITE)
		return (EACCES);

	ks = ksp->ks_data;

	mutex_enter(&sc->mtx);

	ks->re_type.value.ui64 = sc->re_type;
	ks->re_device_id.value.ui64 = sc->re_device_id;
	ks->mtu.value.ui64 = sc->mtu;
	ks->link_state.value.ui64 = sc->re_link_state;
	ks->link_speed.value.ui64 = sc->re_link_speed;
	ks->link_duplex.value.ui64 = sc->re_link_duplex;
	ks->num_rx_rings.value.ui64 = sc->num_rx_rings;
	ks->num_tx_rings.value.ui64 = sc->num_tx_rings;
	ks->re_intr_type.value.ui64 = sc->re_intr_type;
	ks->intr_cnt.value.ui64 = sc->intr_cnt;
	ks->hw_isr_ver.value.ui64 = sc->HwSuppIsrVer;
	ks->suspended.value.ui64 = sc->suspended;

	if (re_dump_tally_counter(sc) != 0) {
		mutex_exit(&sc->mtx);
		return (EIO);
	}

	/*
	 * tally buffer is DMA-backed; sync before reading snapshot.
	 */
	RE_DMA_SYNC(&sc->re_tally.tally_buf, DDI_DMA_SYNC_FORKERNEL);

	st = sc->re_tally.stats;
	if (st == NULL) {
		mutex_exit(&sc->mtx);
		return (EIO);
	}

	ks->rx_pkts.value.ui64 = st->re_rx_pkts;
	ks->tx_pkts.value.ui64 = st->re_tx_pkts;
	ks->rx_octets.value.ui64 = st->re_rx_octets;
	ks->tx_octets.value.ui64 = st->re_tx_octets;
	ks->rx_multicast64.value.ui64 = st->re_rx_multicast64;
	ks->tx_multicast64.value.ui64 = st->re_tx_multicast64;
	ks->rx_bcasts.value.ui64 = st->re_rx_bcasts;
	ks->tx_broadcast64.value.ui64 = st->re_tx_broadcast64;
	ks->rx_errs.value.ui64 = st->re_rx_errs;
	ks->tx_errs.value.ui64 = st->re_tx_errs;
	ks->missed_pkts.value.ui64 = st->re_missed_pkts;
	ks->tx_all_collision.value.ui64 = st->re_tx_all_collision;
	ks->rdu.value.ui64 = st->re_rdu;
	ks->tx_underrun32.value.ui64 = st->re_tx_underrun32;
	ks->rx_framealign_errs.value.ui64 = st->re_rx_framealign_errs;
	ks->tx_onecoll.value.ui64 = st->re_tx_onecoll;
	ks->tx_multicolls.value.ui64 = st->re_tx_multicolls;
	ks->tx_deferred.value.ui64 = st->re_tx_deferred;
	ks->tx_late_collision.value.ui64 = st->re_tx_late_collision;
	ks->rx_frame_too_long.value.ui64 = st->re_rx_frame_too_long;
	ks->rx_runt.value.ui64 = st->re_rx_runt;
	ks->re_reset_counter.value.ui64 = sc->re_reset_counter;
	mutex_exit(&sc->mtx);
	return (0);
}

boolean_t
re_softc_kstat_init(struct re_softc *sc)
{
	kstat_t *ksp;
	re_softc_kstats_t *ks;

	ksp = kstat_create(RE_MOD_NAME, ddi_get_instance(sc->dev),
	    "softc", "net", KSTAT_TYPE_NAMED,
	    sizeof (re_softc_kstats_t) / sizeof (kstat_named_t),
	    0);
	if (ksp == NULL)
		return (B_FALSE);

	sc->sc_ksp = ksp;
	ks = &sc->sc_kstat;

	ksp->ks_data = ks;
	ksp->ks_private = sc;
	ksp->ks_update = re_softc_kstat_update;

	kstat_named_init(&ks->re_type, "re_type", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->re_device_id, "re_device_id", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->mtu, "mtu", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->link_state, "link_state", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->link_speed, "link_speed", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->link_duplex, "link_duplex", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->num_rx_rings, "num_rx_rings", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->num_tx_rings, "num_tx_rings", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->re_intr_type, "re_intr_type", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->intr_cnt, "intr_cnt", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->hw_isr_ver, "HwSuppIsrVer", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->suspended, "suspended", KSTAT_DATA_UINT64);

	kstat_named_init(&ks->rx_pkts, "rx_pkts", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_pkts, "tx_pkts", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_octets, "rx_octets", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_octets, "tx_octets", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_multicast64, "rx_multicast64", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_multicast64, "tx_multicast64", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_bcasts, "rx_bcasts", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_broadcast64, "tx_broadcast64", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_errs, "rx_errs", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_errs, "tx_errs", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->missed_pkts, "missed_pkts", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_all_collision, "tx_all_collision", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rdu, "rdu", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_underrun32, "tx_underrun32", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_framealign_errs, "rx_framealign_errs", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_onecoll, "tx_onecoll", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_multicolls, "tx_multicolls", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_deferred, "tx_deferred", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->tx_late_collision, "tx_late_collision", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_frame_too_long, "rx_frame_too_long", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->rx_runt, "rx_runt", KSTAT_DATA_UINT64);
	kstat_named_init(&ks->re_reset_counter, "re_reset_counter", KSTAT_DATA_UINT64);

	kstat_install(ksp);
	return (B_TRUE);
}

void
re_softc_kstat_fini(struct re_softc *sc)
{
	if (sc->sc_ksp != NULL) {
		kstat_delete(sc->sc_ksp);
		sc->sc_ksp = NULL;
	}
}
