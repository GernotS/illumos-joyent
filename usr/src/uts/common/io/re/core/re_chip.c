/*
 * Copyright (c) 2025 Adrian Chadd <adrian@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "re.h"

static uint32_t re_hw_caps_from_mac_type(struct re_softc *sc);
static void re_set_rx_q_num(struct re_softc *sc, uint16_t num_q);
static void re_set_tx_q_num(struct re_softc *sc, uint16_t num_q);

void
re_stop_locked(struct re_softc *sc)
{
	int ret;

	RE_LOCK_ASSERT(sc);

	sc->sc_stopped = B_TRUE;

	if ((sc->attach_state & RE_ATTACH_MAC_START) == 0)
		return;

    /*
	 * Block packet reception in hardware before reset.
	 */

	RE_CLRBIT_4(sc, RE_RXCFG, RE_RXCFG_ALLPHYS | RE_RXCFG_INDIV |
	    RE_RXCFG_MULTI | RE_RXCFG_BROAD | RE_RXCFG_RUNT |
	    RE_RXCFG_ERRPKT);

	/* exit from this mutex to avoid deadlock */
	mutex_exit(&sc->mtx);
	/*
	 * Stop RX/TX paths, wait for interrupt taskq handlers,
	 * then drain software-visible RX/TX work.
	 *
	 * May wait for taskq.  Must not hold sc->mtx here.
	 */
	ret = re_stop_datapath(sc, drv_usectohz(5000000));

	mutex_enter(&sc->mtx);

	if (ret != 0) {
		dev_err(sc->dev, CE_WARN,
		    "re_stop_datapath failed: %d", ret);
	}

	re_hw_reset(sc);

	/*
	 * Undo runtime interrupt/layer enable if still needed for your path.
	 */
	RE_MAC_CLRBIT(sc, 0xc0ac, 0x1f80);

	sc->re_link_state = LINK_STATE_UNKNOWN;
	sc->re_link_speed = 0;
	sc->re_link_duplex = LINK_DUPLEX_UNKNOWN;
}

static void re_enable_extend_tally_couter(struct re_softc *sc)
{
	RE_MAC_SETBIT(sc, 0xEA84, 0x0003);
}

void re_disable_extend_tally_couter(struct re_softc *sc)
{
	RE_MAC_CLRBIT(sc, 0xEA84, 0x0003);
}

static void
re_hw_config_interrupt_type(struct re_softc *sc)
{
	uint8_t val;

	if (sc->HwSuppIsrVer < 2)
		return;

	val = RE_READ_1(sc, RE_INT_CFG0);

	switch (sc->HwSuppIsrVer) {
	case 7:
		val &= ~RE_INT_CFG0_AVOID_MISS_INTR;
		val &= ~RE_INT_CFG0_AUTO_CLEAR_IMR;
		break;
	case 4:
	case 5:
		val &= ~RE_INT_CFG0_MSIX_ENTRY_NUM_MODE;
		break;
	case 2:
	case 3:
		val &= ~RE_INT_CFG0_EN;
		if (sc->HwCurrIsrVer > 1)
			val |= RE_INT_CFG0_EN;
		break;
	default:
		return;
	}

	RE_WRITE_1(sc, RE_INT_CFG0, val);
}

static void
re_hw_clear_int_miti(struct re_softc *sc)
{
	int i;
	uint8_t v;

	switch (sc->HwSuppIntMitiVer) {
	case 3:
	case 6:
		/* IntMITI_0 - IntMITI_31 */
		for (i = RE_INT_MITI_BASE; i < RE_INT_MITI_END_V3; i += 4)
			RE_WRITE_4(sc, i, 0);
		break;

	case 4:
	case 5:
		/* IntMITI_0 - IntMITI_15 */
		for (i = RE_INT_MITI_BASE; i < RE_INT_MITI_END_V2; i += 4)
			RE_WRITE_4(sc, i, 0);

		v = RE_READ_1(sc, RE_INT_CFG0);
		if (sc->HwSuppIntMitiVer == 5) {
			v &= ~(RE_INT_CFG0_TIMEOUT_BYPASS |
			    RE_INT_CFG0_MITIGATION_BYPASS |
			    RE_INT_CFG0_RDU_BYPASS_8126);
		} else {
			v &= ~(RE_INT_CFG0_TIMEOUT_BYPASS |
			    RE_INT_CFG0_MITIGATION_BYPASS);
		}
		RE_WRITE_1(sc, RE_INT_CFG0, v);

		RE_WRITE_2(sc, RE_INT_CFG1, 0);
		break;
	}
}

static void
re_hw_set_timer_int(struct re_softc *sc, uint8_t timer_val)
{
	int i;

	if (sc->HwSuppIntMitiVer > 3) {
		if ((sc->HwCurrIsrVer == 2))
			timer_val = 0;

		for (i = 0; i < sc->num_rx_rings; i++) {
			re_rx_ring_t *rr = &sc->rx_rings[i];

			if (rr == NULL)
				continue;

			RE_WRITE_1(sc,
				RE_INT_MITI_V2_0_RX + 8 * rr->index,
				timer_val);
		}

		for (i = 0; i < sc->num_tx_rings; i++) {
			re_tx_ring_t *tr = &sc->tx_rings[i];

			if (tr == NULL)
				continue;

			RE_WRITE_1(sc,
				RE_INT_MITI_V2_0_TX + 8 * tr->index,
				timer_val);
		}
	}
}

void
re_init_locked(struct re_softc *sc)
{
	uint32_t rxconf, val;
	RE_ASSERT_LOCKED(sc);

	/* Don't double-init the hardware */
	if (((sc->attach_state & RE_ATTACH_MAC_START) != 0) && !sc->reset_running) {
		return;
	}

	/*
	* Bring the hardware down so we know it's in a good known
	* state before we bring it up in a good known state.
	*/
	re_stop_locked(sc);

	/* Set MAC address. */
	re_set_macaddr(sc, sc->org_mac_addr);

	if (re_chipinit(sc)) {
		return;
	}

	if (re_phy_config(sc))
		return;

	RE_SETBIT_1(sc, RE_EECMD, RE_EECMD_WRITECFG);

	RE_CLRBIT_1(sc, 0xf1, 0x80);
	re_disable_aspm_clkreq(sc);
	RE_WRITE_2(sc, RE_EEE_TXIDLE_TIMER,
		RE_JUMBO_MTU + ETHER_HDR_LEN + 32);

	/* rings mapping to dev */
	re_map_rings_to_dev(sc);

	/* Set the initial RX and TX configurations. */
	if (sc->re_type == MAC_R25)
		rxconf = RE_RXCFG_CONFIG;
	else if (sc->re_type == MAC_R25B)
		rxconf = RE_RXCFG_CONFIG_8125B;
	else if (RE_TYPE_R25D(sc))
		rxconf = RE_RXCFG_CONFIG_8125D;
	else
		rxconf = RE_RXCFG_CONFIG_8126;

	RE_WRITE_4(sc, RE_RXCFG, rxconf);
	RE_WRITE_4(sc, RE_TXCFG, RE_TXCFG_CONFIG);

	re_enable_extend_tally_couter(sc);

	val = re_read_csi(sc, 0x70c) & ~0x3f000000;
	re_write_csi(sc, 0x70c, val | 0x27000000);

	if (RE_TYPE_R26(sc) || sc->re_type == MAC_R27) {
		/* Disable L1 timeout. */
		val = re_read_csi(sc, 0x890) & ~0x00000001;
		re_write_csi(sc, 0x890, val);
	} else if (!RE_TYPE_R25D(sc))
		RE_WRITE_2(sc, 0x0382, 0x221b);

	/* rss config */
	re_config_rss(sc);
	re_set_rx_q_num(sc, sc->num_rx_rings);

	RE_CLRBIT_1(sc, RE_CFG1, RE_CFG1_SPEED_DOWN);

	re_write_mac_ocp(sc, 0xc140, 0xffff);
	re_write_mac_ocp(sc, 0xc142, 0xffff);

	/* new tx desc format */
	RE_MAC_SETBIT(sc, 0xeb58, 0x0001);

	if (RE_TYPE_R26(sc) || sc->re_type == MAC_R27) {
		RE_CLRBIT_1(sc, 0xd8, 0x02);
		if (sc->re_type == MAC_R27) {
			RE_CLRBIT_1(sc, 0x20e4, 0x04);
			RE_MAC_CLRBIT(sc, 0xe00c, 0x1000);
			RE_MAC_CLRBIT(sc, 0xc0c2, 0x0040);
		}
	}

	val = re_read_mac_ocp(sc, 0xe614);
	val &= (sc->re_type == MAC_R27) ? ~0x0f00 : ~0x0700;
	if (sc->re_type == MAC_R25 || RE_TYPE_R25D(sc))
		re_write_mac_ocp(sc, 0xe614, val | 0x0300);
	else if (sc->re_type == MAC_R25B)
		re_write_mac_ocp(sc, 0xe614, val | 0x0200);
	else if (RE_TYPE_R26(sc))
		re_write_mac_ocp(sc, 0xe614, val | 0x0300);
	else
		re_write_mac_ocp(sc, 0xe614, val | 0x0f00);

	/* set tx q num */
	re_set_tx_q_num(sc, sc->num_tx_rings);

	val = re_read_mac_ocp(sc, 0xe63e) & ~0x0030;
	re_write_mac_ocp(sc, 0xe63e, val | 0x0020);

	RE_MAC_CLRBIT(sc, 0xc0b4, 0x0001);
	RE_MAC_SETBIT(sc, 0xc0b4, 0x0001);

	RE_MAC_SETBIT(sc, 0xc0b4, 0x000c);

	val = re_read_mac_ocp(sc, 0xeb6a) & ~0x00ff;
	re_write_mac_ocp(sc, 0xeb6a, val | 0x0033);

	val = re_read_mac_ocp(sc, 0xeb50) & ~0x03e0;
	re_write_mac_ocp(sc, 0xeb50, val | 0x0040);

	RE_MAC_CLRBIT(sc, 0xe056, 0x00f0);

	RE_WRITE_1(sc, RE_TDFNR, 0x10);

	RE_MAC_CLRBIT(sc, 0xe040, 0x1000);

	val = re_read_mac_ocp(sc, 0xea1c) & ~0x0003;
	re_write_mac_ocp(sc, 0xea1c, val | 0x0001);

	if (RE_TYPE_R25D(sc))
		re_write_mac_ocp(sc, 0xe0c0, 0x4403);
	else
		re_write_mac_ocp(sc, 0xe0c0, 0x4000);

	RE_MAC_SETBIT(sc, 0xe052, 0x0060);
	RE_MAC_CLRBIT(sc, 0xe052, 0x0088);

	val = re_read_mac_ocp(sc, 0xd430) & ~0x0fff;
	re_write_mac_ocp(sc, 0xd430, val | 0x045f);

	RE_SETBIT_1(sc, RE_DLLPR, RE_DLLPR_PFM_EN | RE_DLLPR_TX_10M_PS_EN);

	if (sc->re_type == MAC_R25)
		RE_SETBIT_1(sc, RE_MCUCMD, 0x01);

	if (!RE_TYPE_R25D(sc)) {
		/* Disable EEE plus. */
		RE_MAC_CLRBIT(sc, 0xe080, 0x0002);
	}

	if (RE_TYPE_R26(sc) || sc->re_type == MAC_R27)
		RE_MAC_CLRBIT(sc, 0xea1c, 0x0304);
	else
		RE_MAC_CLRBIT(sc, 0xea1c, 0x0004);

	/* Clear tcam entries. */
	RE_MAC_SETBIT(sc, 0xeb54, 0x0001);
	DELAY(1);
	RE_MAC_CLRBIT(sc, 0xeb54, 0x0001);

	RE_CLRBIT_2(sc, 0x1880, 0x0030);

	if (sc->re_rx_ring_desc_type == RE_RX_RING_DESC_TYPE_4) {
		if (sc->Enable_Rss) {
			RE_WRITE_1(sc, 0xd8, RE_READ_1(sc, 0xd8) |
						EnableRxDescV4_0);
		} else {
			RE_WRITE_1(sc, 0xd8, RE_READ_1(sc, 0xd8) &
						~EnableRxDescV4_0);
		}
	}

	if (sc->re_type == MAC_R27) {
		val = re_read_mac_ocp(sc, 0xd40c) & ~0xe038;
		re_write_phy_ocp(sc, 0xd40c, val | 0x8020);
	}

	re_hw_config_interrupt_type(sc);

	/* Clear timer interrupts. */
	RE_WRITE_4(sc, RE_TIMERINT0, 0);
	RE_WRITE_4(sc, RE_TIMERINT1, 0);
	RE_WRITE_4(sc, RE_TIMERINT2, 0);
	RE_WRITE_4(sc, RE_TIMERINT3, 0);

	/* Clear interrupt moderation timer. */
	re_hw_clear_int_miti(sc);

	if (sc->timer_int_enable &&
		sc->HwCurrIsrVer > 1 &&
		sc->HwSuppIntMitiVer > 3 &&
		sc->re_intr_type == DDI_INTR_TYPE_MSIX) {
		re_hw_set_timer_int(sc, sc->timer_count_v2);
	}

	RE_MAC_SETBIT(sc, 0xc0ac, 0x1f80);

	re_write_mac_ocp(sc, 0xe098, 0xc302);

	RE_MAC_CLRBIT(sc, 0xe032, 0x0003);
	val = re_read_csi(sc, 0x98) & ~0x0000ff00;
	re_write_csi(sc, 0x98, val);

	if (RE_TYPE_R25D(sc)) {
		val = re_read_mac_ocp(sc, 0xe092) & ~0x00ff;
		re_write_mac_ocp(sc, 0xe092, val | 0x0008);
	} else
		RE_MAC_CLRBIT(sc, 0xe092, 0x00ff);


	/* stack not support vlan stripping offload, always disable */
	RE_CLRBIT_4(sc, RE_RXCFG, RE_RXCFG_VLANSTRIP);

	/* Enable/disable RX checksum based on enabled capability */
	RE_SETBIT_2(sc, RE_CPLUSCMD, RE_CPLUSCMD_RXCSUM);

	(void) RE_READ_2(sc, RE_CPLUSCMD);

	/* Set Maximum frame size. */
	RE_WRITE_2(sc, RE_RXMAXSIZE, RE_JUMBO_FRAMELEN);

	/* Disable RXDV gate. */
	RE_CLRBIT_1(sc, RE_PPSW, 0x08);
	DELAY(2000);

	/* Program promiscuous mode and multicast filters. */
	re_set_rx_packet_filter(sc);

	if (sc->re_type == MAC_R27)
		RE_CLRBIT_1(sc, RE_RADMFIFO_PROTECT, 0x2001);

	re_disable_aspm_clkreq(sc);

	RE_CLRBIT_1(sc, RE_EECMD, RE_EECMD_WRITECFG);
	DELAY(10);

	(void) re_apply_adv_caps(sc);

	/* Enable transmit and receive. */
	RE_WRITE_1(sc, RE_CMD, RE_CMD_TXENB | RE_CMD_RXENB);

	/* Enable interrupts. */
	re_enable_imr_8125(sc);

	/* Unblock transmit when we release the lock */
	sc->sc_stopped = B_FALSE;
}

#define RE_HASH_POLY 0x04C11DB7U
#define RE_HASH_CRC  0xFFFFFFFFU

uint32_t
re_mcast_hash_index(const uint8_t *mca)
{
	uint32_t crc = RE_HASH_CRC;
	uint32_t msb;
	uint32_t index;
	int bytes, bit;
	uint8_t currentbyte;

	for (bytes = 0; bytes < ETHERADDRL; bytes++) {
		currentbyte = mca[bytes];

		for (bit = 0; bit < 8; bit++) {
			msb = crc >> 31;
			crc <<= 1;

			if ((msb ^ (currentbyte & 1U)) != 0)
				crc ^= RE_HASH_POLY;

			currentbyte >>= 1;
		}
	}

	index = (crc >> 26) & 0x3f;
	return (index);
}

static void
re_set_multicast_reg(struct re_softc *sc, uint32_t mask0, uint32_t mask4)
{
	RE_WRITE_4(sc, RE_MAR0, mask0);
	RE_WRITE_4(sc, RE_MAR4, mask4);
}

static void
re_setmulti(struct re_softc *sc)
{
	uint32_t mask0, mask4;
	uint32_t i;

	/*
	 * promisc/allmulti:
	 * hash table can be all ones, actual RX acceptance is also controlled
	 * by RE_RXCFG_RX_MULTI / RE_RXCFG_RX_ALLPHYS in re_set_rx_packet_filter().
	 */
	if (sc->re_promisc || sc->re_allmulti) {
		re_set_multicast_reg(sc, 0xFFFFFFFFU, 0xFFFFFFFFU);
		return;
	}

	mask0 = 0;
	mask4 = 0;

	for (i = 0; i < 32; i++) {
		if (sc->mcast_refs[i] != 0)
			mask0 |= (1U << i);
	}

	for (i = 32; i < 64; i++) {
		if (sc->mcast_refs[i] != 0)
			mask4 |= (1U << (i - 32));
	}

	re_set_multicast_reg(sc, mask0, mask4);
}

void
re_set_rx_packet_filter(struct re_softc *sc)
{
	uint32_t base_cfg;
	uint32_t rx_mode;
	uint32_t new_cfg;

	base_cfg = RE_READ_4(sc, RE_RXCFG) & 0xff7e5880;

	rx_mode = 0;
	rx_mode |= RE_RXCFG_INDIV;
	rx_mode |= RE_RXCFG_BROAD;
	if (sc->re_promisc) {
		rx_mode |= RE_RXCFG_ALLPHYS |
				RE_RXCFG_MULTI |
				RE_RXCFG_RUNT |
				RE_RXCFG_ERRPKT;
	} else {
		if (sc->re_allmulti || sc->re_mc_count != 0)
			rx_mode |= RE_RXCFG_MULTI;
	}

	if (sc->num_rx_rings > 1) {
		if (sc->re_rx_ring_desc_type == RE_RX_RING_DESC_TYPE_3)
			base_cfg |= EnableRxDescV3;
		else if (sc->re_rx_ring_desc_type == RE_RX_RING_DESC_TYPE_4)
			base_cfg &= ~EnableRxDescV4_1;
	}

	new_cfg = base_cfg | rx_mode;

	RE_WRITE_4(sc, RE_RXCFG, new_cfg);
	re_setmulti(sc);
}

void
re_init_software_variable(struct re_softc *sc)
{
	switch (sc->re_type) {
	case MAC_R25:
		sc->HwSuppIsrVer = 1;
		sc->HwSuppIntMitiVer = 3;
		sc->timer_count_v2 = sc->timer_count / 0x100;
		break;
	case MAC_R25B:
		sc->HwSuppIsrVer = 2;
		sc->HwSuppIntMitiVer = 4;
		sc->timer_count_v2 = sc->timer_count / 0x100;
		break;
	case MAC_R25D_1:
	case MAC_R25D_2:
		sc->HwSuppIsrVer = 5;
		sc->HwSuppIntMitiVer = 6;
		sc->timer_count_v2 = sc->timer_count / 0x200;
		break;
	case MAC_R26_1:
	case MAC_R26_2:
		sc->HwSuppIsrVer = 3;
		sc->HwSuppIntMitiVer = 6;
		sc->timer_count_v2 = sc->timer_count / 0x100;
		break;
	case MAC_R27:
		sc->HwSuppIsrVer = 6;
		sc->HwSuppIntMitiVer = 6;
		sc->timer_count_v2 = sc->timer_count / 0x200;
		break;
	default:
		sc->HwSuppIsrVer = 0;
		sc->HwSuppIntMitiVer = 0;
		break;
	}

	sc->use_new_intr_mapping = (sc->HwSuppIsrVer > 1) ? 1 : 0;
	sc->HwCurrIsrVer = sc->HwSuppIsrVer;

	if (sc->re_type == MAC_R25) {
		sc->HwSuppNumTxQueues = 1;
		sc->HwSuppNumRxQueues = 1;
	} else {
		sc->HwSuppNumTxQueues = 2;
		sc->HwSuppNumRxQueues = 4;
	}

	switch (sc->re_type) {
	case MAC_R25:
	case MAC_R25B:
		sc->re_rx_ring_desc_type = RE_RX_RING_DESC_TYPE_3;
		break;
	case MAC_R25D_1:
	case MAC_R25D_2:
	case MAC_R26_1:
	case MAC_R26_2:
	case MAC_R27:
		sc->re_rx_ring_desc_type = RE_RX_RING_DESC_TYPE_4;
		break;
	default:
		sc->re_rx_ring_desc_type = RE_RX_RING_DESC_TYPE_1;
		break;
	}

	sc->hw_caps = re_hw_caps_from_mac_type(sc);
	sc->adv_caps = sc->hw_caps;
	sc->autoneg = B_TRUE;
	sc->re_flowctrl = LINK_FLOWCTRL_BI;

	re_choose_intr_layout(sc);
	re_init_rss(sc);
	re_setup_interrupt_mask(sc);

	sc->re_link_state = LINK_STATE_UNKNOWN;
}

uint32_t re_get_isr_8125(struct re_softc *sc)
{
	if (sc->use_new_intr_mapping)
		return RE_READ_4(sc, RE_ISR_V2_8125);
	else
		return RE_READ_4(sc, RE_ISR);
}

void re_set_isr_8125(struct re_softc *sc, uint32_t val)
{
	if (sc->use_new_intr_mapping)
		RE_WRITE_4(sc, RE_ISR_V2_8125, val);
	else
		RE_WRITE_4(sc, RE_ISR, val);
}

void
re_enable_imr_8125(struct re_softc *sc)
{
	if (sc->re_intr_type == DDI_INTR_TYPE_MSIX) {
		/*
		 * MSI-X path:
		 * normal IMR programming via mapping style.
		 * Timer mitigation is configured separately by
		 * re_hw_set_timer_int().
		 */
		if (sc->use_new_intr_mapping) {
			RE_WRITE_4(sc, RE_IMR_V2_SET_REG_8125, sc->intr_mask);
			if (sc->HwSuppIsrVer > 3)
				RE_WRITE_4(sc, RE_IMR_V4_L2_SET_REG_8125,
				    sc->intr_mask);
		} else {
			RE_WRITE_4(sc, RE_IMR, sc->intr_mask);
		}
		return;
	}

	/*
	 * MSI path.
	 */
	if (sc->timer_int_enable) {
		/*
		 * MSI + timer uses legacy timer interrupt path.
		 */
		RE_WRITE_4(sc, RE_TIMERINT0, sc->timer_count);
		RE_WRITE_4(sc, RE_TIMERCNT, sc->timer_count);
		RE_WRITE_4(sc, RE_IMR, sc->timer_intr_mask);
		return;
	}

	/*
	 * Plain MSI path.
	 */
	if (sc->use_new_intr_mapping) {
		RE_WRITE_4(sc, RE_IMR_V2_SET_REG_8125, sc->intr_mask);
		if (sc->HwSuppIsrVer > 3)
			RE_WRITE_4(sc, RE_IMR_V4_L2_SET_REG_8125,
			    sc->intr_mask);
	} else {
		RE_WRITE_4(sc, RE_IMR, sc->intr_mask);
	}
}

void
re_disable_imr_8125(struct re_softc *sc)
{
	if (sc->re_intr_type == DDI_INTR_TYPE_MSIX) {
		if (sc->use_new_intr_mapping) {
			RE_WRITE_4(sc, RE_IMR_V2_CLEAR_REG_8125, 0xffffffff);
			if (sc->HwSuppIsrVer > 3)
				RE_WRITE_4(sc, RE_IMR_V4_L2_CLEAR_REG_8125,
				    0xffffffff);
		} else {
			RE_WRITE_4(sc, RE_IMR, 0);
		}

		/*
		 * MSI-X timer mitigation is separate, but old legacy
		 * timer registers should still be off.
		 */
		RE_WRITE_4(sc, RE_TIMERINT0, 0);
		RE_WRITE_4(sc, RE_TIMERCNT, 0);
		return;
	}

	/*
	 * MSI + timer legacy path.
	 */
	if (sc->timer_int_enable) {
		RE_WRITE_4(sc, RE_TIMERINT0, 0);
		RE_WRITE_4(sc, RE_TIMERCNT, 0);
		RE_WRITE_4(sc, RE_IMR, 0);
		return;
	}

	/*
	 * Plain MSI path.
	 */
	if (sc->use_new_intr_mapping) {
		RE_WRITE_4(sc, RE_IMR_V2_CLEAR_REG_8125, 0xffffffff);
		if (sc->HwSuppIsrVer > 3)
			RE_WRITE_4(sc, RE_IMR_V4_L2_CLEAR_REG_8125,
			    0xffffffff);
	} else {
		RE_WRITE_4(sc, RE_IMR, 0);
	}

	RE_WRITE_4(sc, RE_TIMERINT0, 0);
	RE_WRITE_4(sc, RE_TIMERCNT, 0);
}

void
re_clear_hw_isr_v2(struct re_softc *sc, uint32_t intr_idx)
{
	RE_WRITE_4(sc, RE_ISR_V2_8125, (1 << intr_idx));
}

void
re_setup_interrupt_mask(struct re_softc *sc)
{
	int i;

	sc->intr_mask = 0;
	sc->intr_mask_rx = 0;
	sc->intr_mask_tx = 0;
	sc->intr_mask_link = 0;
	sc->intr_mask_err = 0;

	if (sc->HwCurrIsrVer == 7) {
		sc->intr_mask_link = RE_8125D_ISR_LINKCHG;
		sc->intr_mask_tx = RE_8125D_ISR_TXQ0_OK;
		if (sc->num_tx_rings > 1)
			sc->intr_mask_tx |= RE_8125D_ISR_TXQ1_OK;
		for (i = 0; i < sc->num_rx_rings; i++)
			sc->intr_mask_rx |= RE_8125D_ISR_RXQ0_OK << i;
	} else if (sc->HwCurrIsrVer == 6) {
		sc->intr_mask_link = RE_8127_ISR_LINKCHG;
		sc->intr_mask_tx = RE_8127_ISR_TXQ0_OK;
		if (sc->num_tx_rings > 1)
			sc->intr_mask_tx |= RE_8127_ISR_TXQ1_OK;
		for (i = 0; i < sc->num_rx_rings; i++)
			sc->intr_mask_rx |= RE_8127_ISR_RXQ0_OK << i;
	} else if (sc->HwCurrIsrVer == 5) {
		sc->intr_mask_link = RE_8125D_ISR_LINKCHG;
		sc->intr_mask_tx = RE_8125D_ISR_TXQ0_OK;
		if (sc->num_tx_rings > 1)
			sc->intr_mask_tx |= RE_8125D_ISR_TXQ1_OK;
		for (i = 0; i < sc->num_rx_rings; i++)
			sc->intr_mask_rx |= RE_8125D_ISR_RXQ0_OK << i;
	} else if (sc->HwCurrIsrVer == 4) {
		sc->intr_mask_link = RE_8125BP_ISR_LINKCHG;
		for (i = 0; i < max(sc->num_tx_rings, sc->num_rx_rings); i++) {
			uint32_t bit = RE_8125BP_ISR_TRXQ0_OK << i;
			if (i < sc->num_rx_rings)
				sc->intr_mask_rx |= bit;
			if (i < sc->num_tx_rings)
				sc->intr_mask_tx |= bit;
		}
	} else if (sc->HwCurrIsrVer == 3) {
		sc->intr_mask_link = RE_8125B_ISR_LINKCHG;
		for (i = 0; i < max(sc->num_tx_rings, sc->num_rx_rings); i++) {
			uint32_t bit = RE_8125B_ISR_RXQ0_OK << i;
			if (i < sc->num_rx_rings)
				sc->intr_mask_rx |= bit;
			if (i < sc->num_tx_rings)
				sc->intr_mask_tx |= bit;
		}
	} else if (sc->HwCurrIsrVer == 2) {
		sc->intr_mask_link = RE_8125B_ISR_LINKCHG;
		sc->intr_mask_tx = RE_8125B_ISR_TXQ0_OK;
		if (sc->num_tx_rings > 1)
			sc->intr_mask_tx |= RE_8125B_ISR_TXQ1_OK;
		for (i = 0; i < sc->num_rx_rings; i++)
			sc->intr_mask_rx |= RE_8125B_ISR_RXQ0_OK << i;
	} else {
		sc->intr_mask_link = RE_ISR_LINKCHG;
		sc->intr_mask_rx = RE_ISR_RX_DESC_UNAVAIL | RE_ISR_RX_OK;
		sc->intr_mask_tx = RE_ISR_TX_OK;
		sc->intr_mask_err = RE_ISR_SW_INT;
		sc->timer_intr_mask = RE_ISR_LINKCHG | RE_ISR_PCS_TIMEOUT;
	}

	sc->intr_mask = sc->intr_mask_rx |
	    sc->intr_mask_tx |
	    sc->intr_mask_link |
	    sc->intr_mask_err;
}

static void
re_disable_hw_interrupt_v2(struct re_softc *sc,
                                uint32_t message_id)
{
	if (message_id < 32)
		RE_WRITE_4(sc, RE_IMR_V2_CLEAR_REG_8125, 1 << message_id);
	else
		return;
}

static void
re_enable_hw_interrupt_v2(struct re_softc *sc, uint32_t message_id)
{
	if (message_id < 32)
		RE_WRITE_4(sc, RE_IMR_V2_SET_REG_8125, 1 << message_id);
	else
		return;
}

static void
re_disable_hw_l2_interrupt_v4(struct re_softc *sc,
                                   uint32_t message_id)
{
	if (message_id < 32)
		return;
	else
		RE_WRITE_4(sc, RE_IMR_V4_L2_CLEAR_REG_8125, 1 << (message_id - 32));
}

static void
re_enable_hw_l2_interrupt_v4(struct re_softc *sc, uint32_t message_id)
{
	if (message_id < 32)
		return;
	else
		RE_WRITE_4(sc, RE_IMR_V4_L2_SET_REG_8125, 1 << (message_id - 32));
}

void
re_disable_hw_layered_interrupt(struct re_softc *sc,
                                     uint32_t message_id)
{
	if (message_id < 32)
		re_disable_hw_interrupt_v2(sc, message_id);
	else
		re_disable_hw_l2_interrupt_v4(sc, message_id);
}

void
re_enable_hw_layered_interrupt(struct re_softc *sc,
                                    uint32_t message_id)
{
	if (message_id < 32)
		re_enable_hw_interrupt_v2(sc, message_id);
	else
		re_enable_hw_l2_interrupt_v4(sc, message_id);
}

void
re_doorbell(struct re_softc *sc, re_tx_ring_t *ring)
{
	if(sc->re_type < MAC_R27) {
		RE_WRITE_2(sc, RE_TXSTART, (RE_TXSTART_START << ring->index));
	} else {
		RE_WRITE_4(sc, RE_TXSTART, (RE_TXSTART_START << ring->index));
	}
}

static uint16_t _re_read_thermal_sensor(struct re_softc *sc)
{
	uint16_t ts_digout;

	if(sc->re_type > MAC_R25) {
		ts_digout = re_read_phy_ocp(sc, 0xBD84);
		ts_digout &= 0x3ff;
	} else {
		ts_digout = 0xffff;
	}
	return ts_digout;
}

int re_read_thermal_sensor(struct re_softc *sc)
{
	int tmp;

	tmp = _re_read_thermal_sensor(sc);
	if(sc->re_type < MAC_R26_1) {
		if (tmp > 512)
			return (0 - ((512 - (tmp - 512)) / 2));
		else
			return (tmp / 2);
		} else {
			return tmp;
	}
}

void
re_setup_rings_regs(struct re_softc *sc)
{
	int i;

	sc->tx_rings[0].tdsar_reg = 0x20;
	for (i = 1; i < sc->num_tx_rings; i++)
		sc->tx_rings[i].tdsar_reg = (uint16_t)(0x2100 + (i - 1) * 8);

	sc->rx_rings[0].rdsar_reg = 0xe4;
	for (i = 1; i < sc->num_rx_rings; i++)
		sc->rx_rings[i].rdsar_reg = (uint16_t)(0x4000 + (i - 1) * 8);
}

static void
re_set_rx_q_num(struct re_softc *sc, uint16_t num_rx_queues)
{
	uint16_t q_ctrl;
	uint16_t rx_q_num;

	rx_q_num = (uint16_t)ddi_fls(num_rx_queues) - 1;
	rx_q_num &= 0x0007;
	rx_q_num <<= 2;
	q_ctrl = RE_READ_2(sc, RE_RXQUEUE_CTRL);
	q_ctrl &= ~(0x001C);
	q_ctrl |= rx_q_num;
	RE_WRITE_2(sc, RE_RXQUEUE_CTRL, q_ctrl);
}

static void
re_set_tx_q_num(struct re_softc *sc, uint16_t num_tx_queues)
{
	uint16_t mac_ocp_data;

	mac_ocp_data = re_read_mac_ocp(sc, 0xE63E);
	mac_ocp_data &= ~(0x0C00);
	mac_ocp_data |= (((ddi_fls(num_tx_queues) - 1) & 0x03) << 10);
	re_write_mac_ocp(sc, 0xE63E, mac_ocp_data);
}

int re_check_mac_version(struct re_softc *sc)
{
	int error = DDI_SUCCESS;
	uint32_t hwrev;
	hwrev = RE_READ_4(sc, RE_TXCFG) & RE_TXCFG_HWREV;

	switch (hwrev) {
	case 0x60900000: /* 81 */
		sc->re_type = MAC_R25;
		break;
	case 0x64100000: /* 83 */
		sc->re_type = MAC_R25B;
		break;
	case 0x64900000: /* 91 */
		sc->re_type = MAC_R26_1;
		break;
	case 0x64a00000: /* 92 */
		sc->re_type = MAC_R26_2;
		break;
	case 0x68800000: /* 86 */
		sc->re_type = MAC_R25D_1;
		break;
	case 0x68900000: /* 87 */
		sc->re_type = MAC_R25D_2;
		break;
	case 0x6c900000: /* 101 */
		sc->re_type = MAC_R27;
		break;
	default:
		dev_err(sc->dev, CE_NOTE, "unknown version 0x%08x", hwrev);
		error = DDI_FAILURE;
	}

	return error;
}

static uint32_t
re_hw_caps_from_mac_type(struct re_softc *sc)
{
	uint32_t caps;

	caps = RE_CAP_10HDX |
	    RE_CAP_10FDX |
	    RE_CAP_100HDX |
	    RE_CAP_100FDX |
	    RE_CAP_1000FDX;

	switch (sc->re_type) {
	case MAC_R27:
		caps |= RE_CAP_10GFDX;
	/* FALLTHROUGH */
	case MAC_R26_1:
	case MAC_R26_2:
		caps |= RE_CAP_5000FDX;
	/* FALLTHROUGH */
	case MAC_R25:
	case MAC_R25B:
	case MAC_R25D_1:
	case MAC_R25D_2:
		caps |= RE_CAP_2500FDX;
		break;
	default:
		break;
	}

	return (caps);
}

boolean_t
re_adv_caps_valid(uint32_t adv_caps)
{
	uint32_t speed_mask;

	speed_mask = RE_CAP_10HDX |
	    RE_CAP_10FDX |
	    RE_CAP_100HDX |
	    RE_CAP_100FDX |
	    RE_CAP_1000FDX |
	    RE_CAP_2500FDX |
	    RE_CAP_5000FDX |
	    RE_CAP_10GFDX;

	return ((adv_caps & speed_mask) != 0);
}

int
re_apply_adv_caps(struct re_softc *sc)
{
	uint16_t anar, gig, val, bmcr;

	/*
	 * Disable Gigabit Lite / Lite variants.
	 * Keep MAC_R27 / RE_TYPE_R26() logic.
	 */
	RE_PHY_CLRBIT(sc, 0xa428, 0x0200);
	RE_PHY_CLRBIT(sc, 0xa5ea, 0x0001);
	if (RE_TYPE_R26(sc) || sc->re_type == MAC_R27)
		RE_PHY_CLRBIT(sc, 0xa5ea, 0x0007);

	/*
	 * Clear high-speed advertisements according to chip type.
	 */
	val = re_read_phy_ocp(sc, 0xa5d4);
	switch (sc->re_type) {
	case MAC_R27:
		val &= ~RE_ADV_10000TFDX;
		/* FALLTHROUGH */
	case MAC_R26_1:
	case MAC_R26_2:
		val &= ~RE_ADV_5000TFDX;
		/* FALLTHROUGH */
	default:
		val &= ~RE_ADV_2500TFDX;
		break;
	}

	anar = 0;
	gig = 0;

	if ((sc->adv_caps & RE_CAP_10HDX) != 0)
		anar |= MII_ABILITY_10BASE_T;
	if ((sc->adv_caps & RE_CAP_10FDX) != 0)
		anar |= MII_ABILITY_10BASE_T_FD;
	if ((sc->adv_caps & RE_CAP_100HDX) != 0)
		anar |= MII_ABILITY_100BASE_TX;
	if ((sc->adv_caps & RE_CAP_100FDX) != 0)
		anar |= MII_ABILITY_100BASE_TX_FD;
	if ((sc->adv_caps & RE_CAP_1000HDX) != 0)
		gig |= MII_MSCONTROL_1000T;
	if ((sc->adv_caps & RE_CAP_1000FDX) != 0)
		gig |= MII_MSCONTROL_1000T_FD;

	if ((sc->adv_caps & RE_CAP_2500FDX) != 0)
		val |= RE_ADV_2500TFDX;
	if ((sc->adv_caps & RE_CAP_5000FDX) != 0)
		val |= RE_ADV_5000TFDX;
	if ((sc->adv_caps & RE_CAP_10GFDX) != 0)
		val |= RE_ADV_10000TFDX;

	switch (sc->re_flowctrl) {
	case LINK_FLOWCTRL_NONE:
		break;
	case LINK_FLOWCTRL_RX:
	case LINK_FLOWCTRL_TX:
		anar |= MII_ABILITY_ASMPAUSE;
		break;
	case LINK_FLOWCTRL_BI:
		anar |= MII_ABILITY_PAUSE | MII_ABILITY_ASMPAUSE;
		break;
	default:
		break;
	}

	re_write_phy(sc, 0, MII_AN_ADVERT, anar);
	re_write_phy(sc, 0, MII_MSCONTROL, gig);
	re_write_phy_ocp(sc, 0xa5d4, val);

	/*
	 * Conservative policy: always use autoneg and restart it.
	 */
	bmcr = MII_CONTROL_RESET | MII_CONTROL_ANE | MII_CONTROL_RSAN;
	re_write_phy(sc, 0, MII_CONTROL, bmcr);

	return (0);
}

static uint8_t re_link_ok(struct re_softc *sc)
{
	uint8_t	retval;
	retval = (RE_READ_1(sc, RE_PHYSTAT) & RL_PHY_STATUS_LINK_STS) ? 1 : 0;
	return retval;
}

void
re_check_link_status(void *arg)
{
	struct re_softc *sc = arg;
	link_state_t new_state;
	link_duplex_t new_duplex;
	uint16_t new_speed;
	link_flowctrl_t new_flowctrl;
	link_state_t notify_state = LINK_STATE_UNKNOWN;
	uint32_t msr;
	boolean_t changed = B_FALSE;

	if (sc == NULL)
		return;


	if ((sc->attach_state & RE_ATTACH_MAC_START) == 0 || sc->suspended) {
		return;
	}

	if (re_link_ok(sc)) {
		new_state = LINK_STATE_UP;
		new_duplex = LINK_DUPLEX_HALF;
		new_speed = 0;
		new_flowctrl = LINK_FLOWCTRL_NONE;

		msr = RE_READ_4(sc, RE_PHYSTAT);

		if (msr & RL_PHY_STATUS_FULL_DUP)
			new_duplex = LINK_DUPLEX_FULL;

		if (msr & RL_PHY_STATUS_10M)
			new_speed = SPEED_10;
		else if (msr & RL_PHY_STATUS_100M)
			new_speed = SPEED_100;
		else if (msr & RL_PHY_STATUS_1000MF)
			new_speed = SPEED_1000;
		else if (msr & RL_PHY_STATUS_500MF)
			new_speed = SPEED_1000;
		else if (msr & RL_PHY_STATUS_1250MF)
			new_speed = SPEED_1000;
		else if (msr & RL_PHY_STATUS_2500MF)
			new_speed = SPEED_2500;
		else if (msr & RL_PHY_STATUS_5000MF_LITE)
			new_speed = SPEED_2500;
		else if (msr & RL_PHY_STATUS_5000MF)
			new_speed = SPEED_5000;
		else if (msr & RL_PHY_STATUS_10000MF_LITE)
			new_speed = SPEED_5000;
		else if (msr & RL_PHY_STATUS_10000MF)
			new_speed = SPEED_10000;

		if ((msr & RL_PHY_STATUS_TX_FLOW_CTRL) &&
		    (msr & RL_PHY_STATUS_RX_FLOW_CTRL))
			new_flowctrl = LINK_FLOWCTRL_BI;
		else if (msr & RL_PHY_STATUS_TX_FLOW_CTRL)
			new_flowctrl = LINK_FLOWCTRL_TX;
		else if (msr & RL_PHY_STATUS_RX_FLOW_CTRL)
			new_flowctrl = LINK_FLOWCTRL_RX;
	} else {
		new_state = LINK_STATE_DOWN;
		new_duplex = LINK_DUPLEX_UNKNOWN;
		new_speed = 0;
		new_flowctrl = LINK_FLOWCTRL_NONE;
	}

	if (new_state != sc->re_link_state ||
	    new_duplex != sc->re_link_duplex ||
	    new_speed != sc->re_link_speed ||
	    new_flowctrl != sc->re_flowctrl) {
		sc->re_link_state = new_state;
		sc->re_link_duplex = new_duplex;
		sc->re_link_speed = new_speed;
		sc->re_flowctrl = new_flowctrl;
		changed = B_TRUE;
		notify_state = new_state;
	}

	if (changed)
		mac_link_update(sc->mh, notify_state);
}

int re_dump_tally_counter(struct re_softc *sc)
{
	uint32_t WaitCnt;
	int retval = -1;
	RE_DMA_SYNC(&sc->re_tally.tally_buf, DDI_DMA_SYNC_FORDEV);
	RE_WRITE_4(sc, RE_DTCCR_HI, RE_ADDR_HI(sc->re_tally.tally_buf.cookie.dmac_laddress));
	RE_WRITE_4(sc, RE_DTCCR_LO, RE_ADDR_LO(sc->re_tally.tally_buf.cookie.dmac_laddress));
	RE_WRITE_4(sc, RE_DTCCR_LO, RE_ADDR_LO(sc->re_tally.tally_buf.cookie.dmac_laddress) | RE_DTCCR_CMD);

	WaitCnt = 0;
	while (RE_READ_4(sc, RE_DTCCR_LO) & RE_DTCCR_CMD) {
		DELAY(1000);

		WaitCnt++;
		if (WaitCnt > 20)
			break;
	}

	if (WaitCnt <= 20)
		retval = 0;

	return retval;
}
