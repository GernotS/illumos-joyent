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

#include "re.h"


static uint32_t
re_floor_pow2(uint32_t n)
{
	uint32_t p = 1;

	if (n == 0)
		return (1);

	while ((p << 1) != 0 && (p << 1) <= n)
		p <<= 1;

	return (p);
}

static uint32_t
re_count_cpus_for_rss(void)
{
	int ncpu;

	ncpu = ncpus_online;
	if (ncpu <= 0)
		ncpu = 1;

	return ((uint32_t)ncpu);
}

static uint32_t
re_choose_rx_ring_count(struct re_softc *sc, uint32_t ncpu)
{
	uint32_t nrx;
	uint32_t hw_max;

	if (ncpu == 0)
		ncpu = 1;

	hw_max = sc->HwSuppNumRxQueues;
	if (hw_max == 0)
		hw_max = 1;

	/*
	 * Берём минимум из CPU и HW возможностей
	 */
	nrx = MIN(ncpu, hw_max);

	/*
	 * Приводим к степени двойки вниз (для RSS/RETA)
	 */
	nrx = re_floor_pow2(nrx);

	if (nrx == 0)
		nrx = 1;

	return (nrx);
}



static uint32_t
re_calc_msix_min_required(struct re_softc *sc)
{
	switch (sc->HwSuppIsrVer) {
	case 2:
		/* RX: 0..3, TX: 16/18, LINK: 21 */
		return (22);

	case 3:
		/* RXTX: 0..3, LINK: 21 */
		return (22);

	case 4:
		/* RXTX: 0..3, LINK: 29 */
		return (30);

	case 5:
		/* RX: 0..3, TX: 16/17, LINK: 18 */
		return (19);

	case 6:
		/* RX: 0..3, TX: 8/9, LINK: 29 */
		return (30);

	case 7:
		/* RX: 0..3, TX: 27/28, LINK: 29 */
		return (30);

	default:
		return (1);
	}
}

static uint32_t
re_choose_tx_ring_count(uint32_t nrx)
{
	if (nrx >= 4)
		return (2);

	return (1);
}

void
re_choose_intr_layout(struct re_softc *sc)
{
	uint32_t ncpu;
	uint32_t nrx;
	uint32_t ntx;

	ncpu = re_count_cpus_for_rss();

	nrx = re_choose_rx_ring_count(sc, ncpu);
	ntx = re_choose_tx_ring_count(nrx);

	sc->num_rx_rings = nrx;
	sc->num_tx_rings = ntx;
	sc->msix_min_reqired =
	    (uint16_t)re_calc_msix_min_required(sc);
}

uint32_t
re_rss_indir_tbl_entries(struct re_softc *sc)
{
	(void)sc;
	return (RE_MAX_INDIRECTION_TABLE_ENTRIES);
}

static uint32_t
re_rss_key_reg(struct re_softc *sc)
{
	(void)sc;
	return (RSS_KEY_8125);
}

static uint32_t
re_rss_indir_tbl_reg(struct re_softc *sc)
{
	(void)sc;
	return (RSS_INDIRECTION_TBL_8125_V2);
}

static void
re_store_reta(struct re_softc *sc)
{
	uint32_t reg = re_rss_indir_tbl_reg(sc);
	uint32_t i, n = re_rss_indir_tbl_entries(sc), reta = 0;

	for (i = 0; i < n; i++) {
		reta |= ((uint32_t)sc->rss_i_table[i]) << ((i & 3) * 8);

		if ((i & 3) == 3) {
			RE_WRITE_4(sc, reg, reta);
			reg += 4;
			reta = 0;
		}
	}
}

static void
re_store_rss_key(struct re_softc *sc)
{
	uint32_t reg = re_rss_key_reg(sc);
	uint32_t i;

	for (i = 0; i < RE_RSS_KEY_SIZE; i += 4) {
		uint32_t v =
		    ((uint32_t)sc->rss_key[i + 0] << 0) |
		    ((uint32_t)sc->rss_key[i + 1] << 8) |
		    ((uint32_t)sc->rss_key[i + 2] << 16) |
		    ((uint32_t)sc->rss_key[i + 3] << 24);

		RE_WRITE_4(sc, reg + i, v);
	}
}

static uint32_t
re_build_rss_ctrl(struct re_softc *sc)
{
	uint32_t rss_ctrl;
	uint32_t hash_mask_len;
	uint32_t cpu_sel;

	rss_ctrl = 0;

	cpu_sel = ddi_fls(sc->num_rx_rings) - 1;
	cpu_sel &= 0x7;
	rss_ctrl |= cpu_sel << RE_RSS_CPU_NUM_OFFSET;

	hash_mask_len = ddi_fls(re_rss_indir_tbl_entries(sc)) - 1;
	hash_mask_len &= 0x7;
	rss_ctrl |= hash_mask_len << RE_RSS_MASK_BITS_OFFSET;

	rss_ctrl |= RE_RSS_CTRL_TCP_IPV4_SUPP |
	    RE_RSS_CTRL_IPV4_SUPP |
	    RE_RSS_CTRL_TCP_IPV6_SUPP |
	    RE_RSS_CTRL_IPV6_SUPP |
	    RE_RSS_CTRL_IPV6_EXT_SUPP |
	    RE_RSS_CTRL_TCP_IPV6_EXT_SUPP;

	if (sc->rss_ctrl & RE_RSS_FLAG_HASH_UDP_IPV4)
		rss_ctrl |= RE_RSS_CTRL_UDP_IPV4_SUPP;

	if (sc->rss_ctrl & RE_RSS_FLAG_HASH_UDP_IPV6)
		rss_ctrl |= RE_RSS_CTRL_UDP_IPV6_SUPP |
		    RE_RSS_CTRL_UDP_IPV6_EXT_SUPP;

	return (rss_ctrl);
}

void
re_disable_rss(struct re_softc *sc)
{
	RE_WRITE_4(sc, RE_RSS_CTRL, 0x00);
	sc->Enable_Rss = 0;
}

void
re_init_rss(struct re_softc *sc)
{
	uint32_t i, n;

	n = re_rss_indir_tbl_entries(sc);

	for (i = 0; i < n; i++)
		sc->rss_i_table[i] = (uint8_t)(i % sc->num_rx_rings);

	(void)random_get_pseudo_bytes(sc->rss_key, RE_RSS_KEY_SIZE);

	/*
	 * По умолчанию: TCP + IP, UDP выключен.
	 * Если хочешь — можно включить UDP сразу.
	 */
	sc->rss_ctrl = 0;
}

void
re_config_rss(struct re_softc *sc)
{
	uint32_t rss_ctrl;

	if (sc->num_rx_rings <= 1) {
		re_disable_rss(sc);
		return;
	}

	rss_ctrl = re_build_rss_ctrl(sc);

	re_store_reta(sc);
	re_store_rss_key(sc);
	RE_WRITE_4(sc, RE_RSS_CTRL, rss_ctrl);
	sc->Enable_Rss = 1;
}
