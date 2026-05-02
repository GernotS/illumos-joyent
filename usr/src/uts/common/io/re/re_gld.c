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

static mac_ether_media_t
re_link_to_media(struct re_softc *sc)
{
	switch (sc->re_link_state) {
	case LINK_STATE_UP:
		break;
	case LINK_STATE_DOWN:
		return (ETHER_MEDIA_NONE);
	default:
		return (ETHER_MEDIA_UNKNOWN);
	}

	switch (sc->re_link_speed) {
		case SPEED_10:
			return ETHER_MEDIA_10BASE_T;
		case SPEED_100:
			return ETHER_MEDIA_100BASE_TX;
		case SPEED_1000:
			return ETHER_MEDIA_1000BASE_T;
		case SPEED_2500:
			return ETHER_MEDIA_2500BASE_T;
		case SPEED_5000:
			return ETHER_MEDIA_5000BASE_T;
		case SPEED_10000:
			return ETHER_MEDIA_10GBASE_T;
	}

	return (ETHER_MEDIA_UNKNOWN);
}

static int
re_m_getstat(void *arg, uint_t stat, uint64_t *val)
{
	struct re_softc *sc = arg;
	struct re_stats *st;
	int ret = 0;

	*val = 0;

	mutex_enter(&sc->mtx);

	switch (stat) {
	case MAC_STAT_IFSPEED:
		*val = (uint64_t)sc->re_link_speed * 1000000ULL;
		goto out;

	case ETHER_STAT_LINK_DUPLEX:
		*val = sc->re_link_duplex;
		goto out;

	case ETHER_STAT_XCVR_INUSE:
		*val = re_link_to_media(sc);
		goto out;

	case ETHER_STAT_LINK_AUTONEG:
	case ETHER_STAT_CAP_AUTONEG:
	case ETHER_STAT_ADV_CAP_AUTONEG:
		*val = sc->autoneg ? 1 : 0;
		goto out;

	case ETHER_STAT_CAP_10HDX:
		*val = ((sc->hw_caps & RE_CAP_10HDX) != 0);
		goto out;
	case ETHER_STAT_CAP_10FDX:
		*val = ((sc->hw_caps & RE_CAP_10FDX) != 0);
		goto out;
	case ETHER_STAT_CAP_100HDX:
		*val = ((sc->hw_caps & RE_CAP_100HDX) != 0);
		goto out;
	case ETHER_STAT_CAP_100FDX:
		*val = ((sc->hw_caps & RE_CAP_100FDX) != 0);
		goto out;
	case ETHER_STAT_CAP_1000FDX:
		*val = ((sc->hw_caps & RE_CAP_1000FDX) != 0);
		goto out;
	case ETHER_STAT_CAP_2500FDX:
		*val = ((sc->hw_caps & RE_CAP_2500FDX) != 0);
		goto out;
	case ETHER_STAT_CAP_5000FDX:
		*val = ((sc->hw_caps & RE_CAP_5000FDX) != 0);
		goto out;
	case ETHER_STAT_CAP_10GFDX:
		*val = ((sc->hw_caps & RE_CAP_10GFDX) != 0);
		goto out;

	case ETHER_STAT_ADV_CAP_10HDX:
		*val = ((sc->adv_caps & RE_CAP_10HDX) != 0);
		goto out;
	case ETHER_STAT_ADV_CAP_10FDX:
		*val = ((sc->adv_caps & RE_CAP_10FDX) != 0);
		goto out;
	case ETHER_STAT_ADV_CAP_100HDX:
		*val = ((sc->adv_caps & RE_CAP_100HDX) != 0);
		goto out;
	case ETHER_STAT_ADV_CAP_100FDX:
		*val = ((sc->adv_caps & RE_CAP_100FDX) != 0);
		goto out;
	case ETHER_STAT_ADV_CAP_1000FDX:
		*val = ((sc->adv_caps & RE_CAP_1000FDX) != 0);
		goto out;
	case ETHER_STAT_ADV_CAP_2500FDX:
		*val = ((sc->adv_caps & RE_CAP_2500FDX) != 0);
		goto out;
	case ETHER_STAT_ADV_CAP_5000FDX:
		*val = ((sc->adv_caps & RE_CAP_5000FDX) != 0);
		goto out;
	case ETHER_STAT_ADV_CAP_10GFDX:
		*val = ((sc->adv_caps & RE_CAP_10GFDX) != 0);
		goto out;

	case ETHER_STAT_CAP_100T4:
	case ETHER_STAT_CAP_1000HDX:
	case ETHER_STAT_CAP_40GFDX:
	case ETHER_STAT_CAP_25GFDX:
	case ETHER_STAT_CAP_50GFDX:
	case ETHER_STAT_CAP_100GFDX:
	case ETHER_STAT_CAP_200GFDX:
	case ETHER_STAT_CAP_400GFDX:
	case ETHER_STAT_ADV_CAP_100T4:
	case ETHER_STAT_ADV_CAP_1000HDX:
	case ETHER_STAT_ADV_CAP_40GFDX:
	case ETHER_STAT_ADV_CAP_25GFDX:
	case ETHER_STAT_ADV_CAP_50GFDX:
	case ETHER_STAT_ADV_CAP_100GFDX:
	case ETHER_STAT_ADV_CAP_200GFDX:
	case ETHER_STAT_ADV_CAP_400GFDX:
		*val = 0;
		goto out;

	default:
		break;
	}

	if (re_dump_tally_counter(sc) != 0) {
		ret = EIO;
		goto out;
	}

	RE_DMA_SYNC(&sc->re_tally.tally_buf, DDI_DMA_SYNC_FORKERNEL);

	st = sc->re_tally.stats;
	if (st == NULL) {
		ret = EIO;
		goto out;
	}

	switch (stat) {
	case MAC_STAT_IPACKETS:
		*val = st->re_rx_pkts;
		break;
	case MAC_STAT_OPACKETS:
		*val = st->re_tx_pkts;
		break;
	case MAC_STAT_RBYTES:
		*val = st->re_rx_octets;
		break;
	case MAC_STAT_OBYTES:
		*val = st->re_tx_octets;
		break;

	case MAC_STAT_MULTIRCV:
		*val = st->re_rx_multicast64;
		break;
	case MAC_STAT_BRDCSTRCV:
		*val = st->re_rx_bcasts;
		break;
	case MAC_STAT_MULTIXMT:
		*val = st->re_tx_multicast64;
		break;
	case MAC_STAT_BRDCSTXMT:
		*val = st->re_tx_broadcast64;
		break;

	case MAC_STAT_IERRORS:
		*val = st->re_rx_errs;
		break;
	case MAC_STAT_OERRORS:
		*val = st->re_tx_errs;
		break;

	case MAC_STAT_NORCVBUF:
		*val = st->re_missed_pkts;
		break;

	case MAC_STAT_COLLISIONS:
		*val = st->re_tx_all_collision;
		break;

	case MAC_STAT_OVERFLOWS:
		*val = st->re_rdu;
		break;

	case MAC_STAT_UNDERFLOWS:
		*val = st->re_tx_underrun32;
		break;

	case ETHER_STAT_ALIGN_ERRORS:
		*val = st->re_rx_framealign_errs;
		break;

	case ETHER_STAT_FIRST_COLLISIONS:
		*val = st->re_tx_onecoll;
		break;

	case ETHER_STAT_MULTI_COLLISIONS:
		*val = st->re_tx_multicolls;
		break;

	case ETHER_STAT_DEFER_XMTS:
		*val = st->re_tx_deferred;
		break;

	case ETHER_STAT_TX_LATE_COLLISIONS:
		*val = st->re_tx_late_collision;
		break;

	case ETHER_STAT_TOOLONG_ERRORS:
		*val = st->re_rx_frame_too_long;
		break;

	case ETHER_STAT_TOOSHORT_ERRORS:
		*val = st->re_rx_runt;
		break;

	case ETHER_STAT_FCS_ERRORS:
	case ETHER_STAT_EX_COLLISIONS:
	case ETHER_STAT_MACXMT_ERRORS:
	case ETHER_STAT_CARRIER_ERRORS:
	case ETHER_STAT_JABBER_ERRORS:
	case ETHER_STAT_XCVR_ADDR:
	case ETHER_STAT_XCVR_ID:
	case ETHER_STAT_LP_CAP_10HDX:
	case ETHER_STAT_LP_CAP_10FDX:
	case ETHER_STAT_LP_CAP_100HDX:
	case ETHER_STAT_LP_CAP_100FDX:
	case ETHER_STAT_LP_CAP_1000HDX:
	case ETHER_STAT_LP_CAP_1000FDX:
	case ETHER_STAT_LP_CAP_2500FDX:
	case ETHER_STAT_LP_CAP_5000FDX:
	case ETHER_STAT_LP_CAP_10GFDX:
	case ETHER_STAT_LP_CAP_AUTONEG:
	case ETHER_STAT_LP_CAP_PAUSE:
	case ETHER_STAT_LP_CAP_ASMPAUSE:
	case ETHER_STAT_LINK_PAUSE:
	case ETHER_STAT_LINK_ASMPAUSE:
	case ETHER_STAT_CAP_PAUSE:
	case ETHER_STAT_CAP_ASMPAUSE:
	case ETHER_STAT_ADV_CAP_PAUSE:
	case ETHER_STAT_ADV_CAP_ASMPAUSE:
	case ETHER_STAT_CAP_REMFAULT:
	case ETHER_STAT_ADV_REMFAULT:
	case ETHER_STAT_LP_REMFAULT:
	case ETHER_STAT_LP_CAP_25GFDX:
	case ETHER_STAT_LP_CAP_50GFDX:
	case ETHER_STAT_LP_CAP_100GFDX:
	case ETHER_STAT_LP_CAP_200GFDX:
	case ETHER_STAT_LP_CAP_400GFDX:
		ret = ENOTSUP;
		break;

	default:
		ret = ENOTSUP;
		break;
	}

out:
	mutex_exit(&sc->mtx);
	return (ret);
}

static int
re_m_start(void *arg)
{
	struct re_softc *sc = arg;
	int ret;
	mutex_enter(&sc->mtx);

	if ((sc->attach_state & RE_ATTACH_MAC_START) != 0) {
		mutex_exit(&sc->mtx);
		return (0);
	}

	ret = re_alloc_rings_data(sc);
	if (ret != DDI_SUCCESS) {
		mutex_exit(&sc->mtx);
		return (EIO);
	}

	re_start_datapath(sc);

	sc->sc_stopped = B_FALSE;
	sc->attach_state |= RE_ATTACH_MAC_START;

	mutex_exit(&sc->mtx);
	return (0);
}

static void
re_m_stop(void *arg)
{
	struct re_softc *sc = arg;

	mutex_enter(&sc->mtx);

	if ((sc->attach_state & RE_ATTACH_MAC_START) == 0) {
		mutex_exit(&sc->mtx);
		return;
	}

	/* stop rx interrupts, and wait all interrupts tasks to be done */
	re_stop_locked(sc);

	/* free allocated rings buffers */
	re_free_rings_data(sc);

	sc->attach_state &= ~RE_ATTACH_MAC_START;

	mutex_exit(&sc->mtx);
}

static int
re_m_setpromisc(void *drv, boolean_t en)
{
	struct re_softc *sc = drv;
	mutex_enter(&sc->mtx);

	sc->re_promisc = en ? B_TRUE : B_FALSE;

	if (!sc->suspended &&
	    ((sc->attach_state & RE_ATTACH_REGS_MAP) != 0)) {
		re_set_rx_packet_filter(sc);
	}

	mutex_exit(&sc->mtx);
	return (0);
}

static int
re_m_multicast(void *drv, boolean_t add, const uint8_t *mac)
{
	struct re_softc *sc = drv;
	uint32_t bit;
	if (!ETHER_IS_MULTICAST(mac))
		return (EINVAL);

	bit = re_mcast_hash_index(mac);

	mutex_enter(&sc->mtx);

	if (add) {
		if (sc->mcast_refs[bit] != CHAR_MAX) {
			if (sc->mcast_refs[bit] == 0)
				sc->mcast_hash[bit >> 3] |=
				    (uint8_t)(1U << (bit & 7));

			sc->mcast_refs[bit]++;
		}

		if (sc->re_mc_count != UINT32_MAX)
			sc->re_mc_count++;
	} else {
		if (sc->mcast_refs[bit] != 0) {
			sc->mcast_refs[bit]--;

			if (sc->mcast_refs[bit] == 0)
				sc->mcast_hash[bit >> 3] &=
				    (uint8_t)~(1U << (bit & 7));

			if (sc->re_mc_count != 0)
				sc->re_mc_count--;
		}
	}

	if (!sc->suspended &&
	    ((sc->attach_state & RE_ATTACH_REGS_MAP) != 0)) {
		re_set_rx_packet_filter(sc);
	}

	mutex_exit(&sc->mtx);
	return (0);
}

static int
re_rx_ring_start(mac_ring_driver_t rh, uint64_t mr_gen_num)
{
	re_rx_ring_t *rr = (re_rx_ring_t *)rh;
	mutex_enter(&rr->rx_lock);
	rr->ring_gen = mr_gen_num;
	mutex_exit(&rr->rx_lock);
	return (0);
}

mblk_t *
re_rx_poll(void *arg, int poll_bytes)
{
	re_rx_ring_t *rr = arg;
	mblk_t *mp;
	ASSERT3S(poll_bytes, >, 0);

	if (poll_bytes == 0) {
		return (NULL);
	}
	mutex_enter(&rr->rx_lock);
	mp = re_ring_rx(rr, poll_bytes);
	mutex_exit(&rr->rx_lock);
	return (mp);
}

static int
re_rx_ring_stat(mac_ring_driver_t rh, uint_t stat, uint64_t *val)
{
	re_rx_ring_t *rr = (re_rx_ring_t *)rh;

	switch (stat) {
	case MAC_STAT_RBYTES:
		*val = rr->rx_kstat.rbytes64.value.ui64;
		return (0);
	case MAC_STAT_IPACKETS:
		*val = rr->rx_kstat.ipackets64.value.ui64;
		return (0);
	default:
		return (ENOTSUP);
	}
}

static int
re_tx_ring_stat(mac_ring_driver_t rh, uint_t stat, uint64_t *val)
{
	re_tx_ring_t *tr = (re_tx_ring_t *)rh;

	switch (stat) {
	case MAC_STAT_OBYTES:
		*val = tr->tx_kstat.obytes64.value.ui64;
		return (0);
	case MAC_STAT_OPACKETS:
		*val = tr->tx_kstat.opackets64.value.ui64;
		return (0);
	default:
		return (ENOTSUP);
	}
}

static void
re_fill_tx_ring(void *arg, mac_ring_type_t rtype, const int group_index,
    const int ring_index, mac_ring_info_t *infop, mac_ring_handle_t rh)
{
	struct re_softc *sc = arg;
	re_tx_ring_t *tr;

	ASSERT3S(group_index, ==, -1);
	ASSERT3S(ring_index, <, sc->num_tx_rings);

	tr = &sc->tx_rings[ring_index];
	tr->th = rh;

	infop->mri_driver = (mac_ring_driver_t)tr;
	infop->mri_start = NULL;
	infop->mri_stop = NULL;
	infop->mri_tx = re_ring_tx;
	infop->mri_stat = re_tx_ring_stat;
}

static int
re_rx_ring_intr_enable(mac_intr_handle_t ih)
{
	re_rx_ring_t *rr = (re_rx_ring_t *)ih;
	struct re_softc *sc = rr->sc;

	mutex_enter(&sc->mtx);

	if (!sc->suspended &&
	    (sc->attach_state & RE_ATTACH_MAC_START) != 0) {
		re_enable_hw_layered_interrupt(sc, rr->intr_idx);
	}

	mutex_exit(&sc->mtx);
	return (0);
}

static int
re_rx_ring_intr_disable(mac_intr_handle_t ih)
{
	re_rx_ring_t *rr = (re_rx_ring_t *)ih;
	struct re_softc *sc = rr->sc;

	mutex_enter(&sc->mtx);
	re_disable_hw_layered_interrupt(sc, rr->intr_idx);
	mutex_exit(&sc->mtx);

	return (0);
}

static void
re_fill_rx_ring(void *arg, mac_ring_type_t rtype, const int group_index,
    const int ring_index, mac_ring_info_t *infop, mac_ring_handle_t rh)
{
	struct re_softc *sc = arg;
	re_rx_ring_t *rr;

	ASSERT3S(group_index, ==, 0);
	ASSERT3S(ring_index, <, sc->num_rx_rings);

	rr = &sc->rx_rings[ring_index];
	rr->rh = rh;

	infop->mri_driver = (mac_ring_driver_t)rr;
	infop->mri_start = re_rx_ring_start;
	infop->mri_stop = NULL;
	infop->mri_poll = re_rx_poll;
	infop->mri_stat = re_rx_ring_stat;

	infop->mri_intr.mi_handle = (mac_intr_handle_t)rr;
	infop->mri_intr.mi_enable = re_rx_ring_intr_enable;
	infop->mri_intr.mi_disable = re_rx_ring_intr_disable;

	if (sc->re_intr_type == DDI_INTR_TYPE_MSIX)
		infop->mri_intr.mi_ddi_handle = rr->intr_handle;
}

static int
re_group_add_mac(void *gr_drv, const uint8_t *mac)
{
	struct re_softc *sc = gr_drv;

	/* Realtek chips support only one hardware mac address,
	 * so filtering not support by hardware,
	 * but required for mac stack,
	 * in case of implementation of multi-ring architecture.
	 * If you add a VNIC over the interface of this driver,
	 * the chip will go into promisc mode.
	 * The function should always return 0 when the stack is pushed to a hardware MAC address.
	 */

	if (bcmp(mac, sc->org_mac_addr, ETHERADDRL) == 0) {
		return (0);
	}

	return (ENOSPC);
}

static int
re_group_rem_mac(void *gr_drv, const uint8_t *mac)
{
	struct re_softc *sc = gr_drv;
	if (bcmp(mac, sc->org_mac_addr, ETHERADDRL) == 0)
		return (0);

	return (ENOENT);
}

static void
re_fill_rx_group(void *arg, mac_ring_type_t rtype, const int group_index,
    mac_group_info_t *infop, mac_group_handle_t gh)
{
	struct re_softc *sc = arg;

	if (rtype != MAC_RING_TYPE_RX) {
		return;
	}

	sc->re_rxg_hdl = gh;
	infop->mgi_driver = (mac_group_driver_t)sc;
	infop->mgi_start = NULL;
	infop->mgi_stop = NULL;
	infop->mgi_addmac = re_group_add_mac;
	infop->mgi_remmac = re_group_rem_mac;
	infop->mgi_addvlan = NULL;
	infop->mgi_remvlan = NULL;
	infop->mgi_count = sc->num_rx_rings;
}

static boolean_t
re_m_getcapab(void *drv, mac_capab_t capab, void *data)
{
	struct re_softc *sc = drv;
	mac_capab_rings_t *rings;
	uint32_t *cksump;

	switch (capab) {
	case MAC_CAPAB_RINGS:
		rings = data;
		rings->mr_group_type = MAC_GROUP_TYPE_STATIC;
		switch (rings->mr_type) {
		case MAC_RING_TYPE_TX:
			rings->mr_gnum = 0;
			rings->mr_rnum = sc->num_tx_rings;
			rings->mr_rget = re_fill_tx_ring;
			rings->mr_gget = NULL;
			rings->mr_gaddring = NULL;
			rings->mr_gremring = NULL;
			break;

		case MAC_RING_TYPE_RX:
			rings->mr_gnum = 1;
			rings->mr_rnum = sc->num_rx_rings;
			rings->mr_rget = re_fill_rx_ring;
			rings->mr_gget = re_fill_rx_group;
			rings->mr_gaddring = NULL;
			rings->mr_gremring = NULL;
			break;

		default:
			return (B_FALSE);
		}
		break;

	case MAC_CAPAB_HCKSUM:
		cksump = data;
		*cksump = HCKSUM_INET_FULL_V4 | HCKSUM_IPHDRCKSUM;
		break;

	case MAC_CAPAB_LSO:
		return (B_FALSE);

	default:
		return (B_FALSE);
	}

	return (B_TRUE);
}

static uint32_t
re_prop_to_cap(mac_prop_id_t prop)
{
	switch (prop) {
	case MAC_PROP_ADV_10HDX_CAP:
	case MAC_PROP_EN_10HDX_CAP:
		return (RE_CAP_10HDX);

	case MAC_PROP_ADV_10FDX_CAP:
	case MAC_PROP_EN_10FDX_CAP:
		return (RE_CAP_10FDX);

	case MAC_PROP_ADV_100HDX_CAP:
	case MAC_PROP_EN_100HDX_CAP:
		return (RE_CAP_100HDX);

	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_EN_100FDX_CAP:
		return (RE_CAP_100FDX);

	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_EN_1000FDX_CAP:
		return (RE_CAP_1000FDX);

	case MAC_PROP_ADV_2500FDX_CAP:
	case MAC_PROP_EN_2500FDX_CAP:
		return (RE_CAP_2500FDX);

	case MAC_PROP_ADV_5000FDX_CAP:
	case MAC_PROP_EN_5000FDX_CAP:
		return (RE_CAP_5000FDX);

	case MAC_PROP_ADV_10GFDX_CAP:
	case MAC_PROP_EN_10GFDX_CAP:
		return (RE_CAP_10GFDX);

	default:
		return (0);
	}
}

void
re_m_propinfo(void *drv, const char *name, mac_prop_id_t prop,
    mac_prop_info_handle_t prh)
{
	struct re_softc *sc = drv;
	uint32_t cap;

	(void)name;

	switch (prop) {
	case MAC_PROP_DUPLEX:
	case MAC_PROP_SPEED:
	case MAC_PROP_STATUS:
	case MAC_PROP_MEDIA:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		break;

	case MAC_PROP_AUTONEG:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		mac_prop_info_set_default_uint8(prh, 1);
		break;

	case MAC_PROP_MTU:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_RW);
		mac_prop_info_set_range_uint32(prh, ETHERMIN,
		    RE_JUMBO_MTU);
		mac_prop_info_set_default_uint32(prh, ETHERMTU);
		break;

	case MAC_PROP_FLOWCTRL:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_RW);
		mac_prop_info_set_default_link_flowctrl(prh, LINK_FLOWCTRL_BI);
		break;

	case MAC_PROP_ADV_10HDX_CAP:
	case MAC_PROP_ADV_10FDX_CAP:
	case MAC_PROP_ADV_100HDX_CAP:
	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_ADV_2500FDX_CAP:
	case MAC_PROP_ADV_5000FDX_CAP:
	case MAC_PROP_ADV_10GFDX_CAP:
		cap = re_prop_to_cap(prop);
		if ((sc->hw_caps & cap) != 0) {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
			mac_prop_info_set_default_uint8(prh, 1);
		}
		break;

	case MAC_PROP_EN_10HDX_CAP:
	case MAC_PROP_EN_10FDX_CAP:
	case MAC_PROP_EN_100HDX_CAP:
	case MAC_PROP_EN_100FDX_CAP:
	case MAC_PROP_EN_1000FDX_CAP:
	case MAC_PROP_EN_2500FDX_CAP:
	case MAC_PROP_EN_5000FDX_CAP:
	case MAC_PROP_EN_10GFDX_CAP:
		cap = re_prop_to_cap(prop);
		if ((sc->hw_caps & cap) != 0) {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_RW);
			mac_prop_info_set_default_uint8(prh,
			    ((sc->hw_caps & cap) != 0) ? 1 : 0);
		}
		break;

	default:
		break;
	}
}

int
re_m_getprop(void *drv, const char *name, mac_prop_id_t prop,
    uint_t pr_valsize, void *pr_val)
{
	struct re_softc *sc = drv;
	int ret = 0;
	uint8_t *u8p;
	uint32_t *u32p;
	uint64_t u64;
	mac_ether_media_t media;
	uint32_t cap;

	(void)name;

	mutex_enter(&sc->mtx);

	switch (prop) {
	case MAC_PROP_DUPLEX:
		if (pr_valsize < sizeof (link_duplex_t)) {
			ret = EOVERFLOW;
			break;
		}
		bcopy(&sc->re_link_duplex, pr_val, sizeof (link_duplex_t));
		break;

	case MAC_PROP_SPEED:
		if (pr_valsize < sizeof (uint64_t)) {
			ret = EOVERFLOW;
			break;
		}
		u64 = (uint64_t)sc->re_link_speed * 1000000ULL;
		bcopy(&u64, pr_val, sizeof (uint64_t));
		break;

	case MAC_PROP_STATUS:
		if (pr_valsize < sizeof (link_state_t)) {
			ret = EOVERFLOW;
			break;
		}
		bcopy(&sc->re_link_state, pr_val, sizeof (link_state_t));
		break;

	case MAC_PROP_MEDIA:
		if (pr_valsize < sizeof (mac_ether_media_t)) {
			ret = EOVERFLOW;
			break;
		}
		media = re_link_to_media(sc);
		bcopy(&media, pr_val, sizeof (mac_ether_media_t));
		break;

	case MAC_PROP_AUTONEG:
		if (pr_valsize < sizeof (uint8_t)) {
			ret = EOVERFLOW;
			break;
		}
		u8p = pr_val;
		*u8p = sc->autoneg ? 1 : 0;
		break;

	case MAC_PROP_MTU:
		if (pr_valsize < sizeof (uint32_t)) {
			ret = EOVERFLOW;
			break;
		}
		u32p = pr_val;
		*u32p = sc->mtu;
		break;

	case MAC_PROP_FLOWCTRL:
		if (pr_valsize < sizeof (link_flowctrl_t)) {
			ret = EOVERFLOW;
			break;
		}
		bcopy(&sc->re_flowctrl, pr_val, sizeof (link_flowctrl_t));
		break;

	case MAC_PROP_ADV_10HDX_CAP:
	case MAC_PROP_ADV_10FDX_CAP:
	case MAC_PROP_ADV_100HDX_CAP:
	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_ADV_2500FDX_CAP:
	case MAC_PROP_ADV_5000FDX_CAP:
	case MAC_PROP_ADV_10GFDX_CAP:
		if (pr_valsize < sizeof (uint8_t)) {
			ret = EOVERFLOW;
			break;
		}
		cap = re_prop_to_cap(prop);
		u8p = pr_val;
		*u8p = ((sc->hw_caps & cap) != 0) ? 1 : 0;
		break;

	case MAC_PROP_EN_10HDX_CAP:
	case MAC_PROP_EN_10FDX_CAP:
	case MAC_PROP_EN_100HDX_CAP:
	case MAC_PROP_EN_100FDX_CAP:
	case MAC_PROP_EN_1000FDX_CAP:
	case MAC_PROP_EN_2500FDX_CAP:
	case MAC_PROP_EN_5000FDX_CAP:
	case MAC_PROP_EN_10GFDX_CAP:
		if (pr_valsize < sizeof (uint8_t)) {
			ret = EOVERFLOW;
			break;
		}
		cap = re_prop_to_cap(prop);
		u8p = pr_val;
		*u8p = ((sc->adv_caps & cap) != 0) ? 1 : 0;
		break;

	default:
		ret = ENOTSUP;
		break;
	}

	mutex_exit(&sc->mtx);
	return (ret);
}

int
re_m_setprop(void *drv, const char *name, mac_prop_id_t prop, uint_t size,
    const void *val)
{
	struct re_softc *sc = drv;
	int ret = 0;
	boolean_t update_link = B_TRUE;
	uint32_t mtu;
	uint8_t en;
	uint32_t cap;
	link_flowctrl_t fc;
	uint32_t new_adv;

	(void)name;

	mutex_enter(&sc->mtx);

	switch (prop) {
	case MAC_PROP_DUPLEX:
	case MAC_PROP_SPEED:
	case MAC_PROP_STATUS:
	case MAC_PROP_MEDIA:
	case MAC_PROP_ADV_10HDX_CAP:
	case MAC_PROP_ADV_10FDX_CAP:
	case MAC_PROP_ADV_100HDX_CAP:
	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_ADV_2500FDX_CAP:
	case MAC_PROP_ADV_5000FDX_CAP:
	case MAC_PROP_ADV_10GFDX_CAP:
		ret = ENOTSUP;
		break;

	case MAC_PROP_AUTONEG:
		if (size < sizeof (uint8_t)) {
			ret = EINVAL;
			break;
		}

		bcopy(val, &en, sizeof (en));

		if (en == 0) {
			ret = ENOTSUP;
			break;
		}

		sc->autoneg = B_TRUE;
		break;

	case MAC_PROP_MTU:
		if (size < sizeof (uint32_t)) {
			ret = EINVAL;
			break;
		}

		bcopy(val, &mtu, sizeof (mtu));
		ret = mac_maxsdu_update(sc->mh, mtu);
		update_link = B_FALSE;
		break;

	case MAC_PROP_FLOWCTRL:
		if (size < sizeof (link_flowctrl_t)) {
			ret = EINVAL;
			break;
		}

		bcopy(val, &fc, sizeof (fc));

		switch (fc) {
		case LINK_FLOWCTRL_NONE:
		case LINK_FLOWCTRL_RX:
		case LINK_FLOWCTRL_TX:
		case LINK_FLOWCTRL_BI:
			sc->re_flowctrl = fc;
			break;
		default:
			ret = EINVAL;
			break;
		}
		break;

	case MAC_PROP_EN_10HDX_CAP:
	case MAC_PROP_EN_10FDX_CAP:
	case MAC_PROP_EN_100HDX_CAP:
	case MAC_PROP_EN_100FDX_CAP:
	case MAC_PROP_EN_1000FDX_CAP:
	case MAC_PROP_EN_2500FDX_CAP:
	case MAC_PROP_EN_5000FDX_CAP:
	case MAC_PROP_EN_10GFDX_CAP:
		if (size < sizeof (uint8_t)) {
			ret = EINVAL;
			break;
		}

		cap = re_prop_to_cap(prop);
		if (cap == 0) {
			ret = ENOTSUP;
			break;
		}

		if ((sc->hw_caps & cap) == 0) {
			ret = ENOTSUP;
			break;
		}

		bcopy(val, &en, sizeof (en));

		new_adv = sc->adv_caps;
		if (en != 0)
			new_adv |= cap;
		else
			new_adv &= ~cap;

		if (!re_adv_caps_valid(new_adv)) {
			ret = EINVAL;
			break;
		}

		sc->adv_caps = new_adv;
		break;

	default:
		ret = ENOTSUP;
		break;
	}

	if (ret == 0 && update_link)
		ret = re_apply_adv_caps(sc);

	mutex_exit(&sc->mtx);
	return (ret);
}

static mac_callbacks_t re_mac_callbacks = {
	.mc_callbacks = MC_GETCAPAB | MC_GETPROP | MC_SETPROP | MC_PROPINFO,
	.mc_getstat = re_m_getstat,
	.mc_start = re_m_start,
	.mc_stop = re_m_stop,
	.mc_setpromisc = re_m_setpromisc,
	.mc_multicst = re_m_multicast,
	.mc_getcapab = re_m_getcapab,
	.mc_setprop = re_m_setprop,
	.mc_getprop = re_m_getprop,
	.mc_propinfo = re_m_propinfo
};

boolean_t
re_mac_register(struct re_softc *sc)
{
	int ret;
	mac_register_t *mac = mac_alloc(MAC_VERSION);

	if (mac == NULL) {
		dev_err(sc->dev, CE_WARN, "failed to allocate mac "
			"registration handle");
		return (B_FALSE);
	}

	mac->m_type_ident = MAC_PLUGIN_IDENT_ETHER;
	mac->m_driver = sc;
	mac->m_dip = sc->dev;
	mac->m_src_addr = sc->org_mac_addr;
	mac->m_callbacks = &re_mac_callbacks;
	mac->m_min_sdu = 0;
	mac->m_max_sdu = sc->mtu;
	mac->m_margin = VLAN_TAGSZ;
	mac->m_priv_props = NULL;
	mac->m_v12n = MAC_VIRT_LEVEL1;

	ret = mac_register(mac, &sc->mh);
	mac_free(mac);
	if (ret != 0) {
		dev_err(sc->dev, CE_WARN, "failed to register with MAC: "
			"%d", ret);
		return (B_FALSE);
	}

	return (B_TRUE);
}
