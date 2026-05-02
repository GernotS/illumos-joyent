/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019, 2020, 2023-2025 Kevin Lo <kevlo@openbsd.org>
 * Copyright (c) 2025 Adrian Chadd <adrian@FreeBSD.org>
 *
 * Hardware programming portions from Realtek Semiconductor.
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

/*	$OpenBSD: if_rge.c,v 1.38 2025/09/19 00:41:14 kevlo Exp $	*/

#include "re.h"
#include "if_re_microcode.h"

static	int re_reset(struct re_softc *sc);
static	void re_set_phy_power(struct re_softc *sc, int on);
static	void re_mac_config_ext_mcu(struct re_softc *, enum re_mac_type);
static	uint64_t re_mcu_get_bin_version(const uint16_t *, uint16_t);
static	void re_mcu_set_version(struct re_softc *sc, uint64_t mcodever);
static	void re_ephy_config_mac_r25(struct re_softc *sc);
static	void re_ephy_config_mac_r25b(struct re_softc *sc);
static	void re_ephy_config_mac_r27(struct re_softc *sc);
static	void re_phy_config_mac_r27(struct re_softc *sc);
static	void re_phy_config_mac_r26_1(struct re_softc *sc);
static	void re_phy_config_mac_r26_2(struct re_softc *sc);
static	void re_phy_config_mac_r25(struct re_softc *sc);
static	void re_phy_config_mac_r25b(struct re_softc *sc);
static	void re_phy_config_mac_r25d_1(struct re_softc *);
static	void re_phy_config_mac_r25d_2(struct re_softc *);
static	void re_phy_config_mcu(struct re_softc *sc, uint16_t rcodever);
static	void re_hw_init(struct re_softc *sc);
static	void re_disable_phy_ocp_pwrsave(struct re_softc *sc);
static	void re_patch_phy_mcu(struct re_softc *sc, int set);
static	void re_switch_mcu_ram_page(struct re_softc *sc, int page);
static	int re_exit_oob(struct re_softc *sc);
static	void re_write_ephy(struct re_softc *sc, uint16_t reg, uint16_t val);
static	uint16_t re_read_ephy(struct re_softc *sc, uint16_t reg);
static	uint16_t re_check_ephy_ext_add(struct re_softc *sc, uint16_t reg);
static	void re_r27_write_ephy(struct re_softc *sc, uint16_t reg, uint16_t val);


static int
re_reset(struct re_softc *sc)
{
	int i;

	RE_CLRBIT_4(sc, RE_RXCFG, RE_RXCFG_ALLPHYS | RE_RXCFG_INDIV |
	    RE_RXCFG_MULTI | RE_RXCFG_BROAD | RE_RXCFG_RUNT |
	    RE_RXCFG_ERRPKT);

	/* Enable RXDV gate. */
	RE_SETBIT_1(sc, RE_PPSW, 0x08);

	RE_SETBIT_1(sc, RE_CMD, RE_CMD_STOPREQ);
	DELAY(200);

	for (i = 0; i < 3000; i++) {
		DELAY(50);
		if ((RE_READ_1(sc, RE_MCUCMD) & (RE_MCUCMD_RXFIFO_EMPTY |
		    RE_MCUCMD_TXFIFO_EMPTY)) == (RE_MCUCMD_RXFIFO_EMPTY |
		    RE_MCUCMD_TXFIFO_EMPTY))
			break;
	}
	if (sc->re_type != MAC_R25) {
		for (i = 0; i < 3000; i++) {
			DELAY(50);
			if ((RE_READ_2(sc, RE_IM) & 0x0103) == 0x0103)
				break;
		}
	}

	RE_WRITE_1(sc, RE_CMD,
	    RE_READ_1(sc, RE_CMD) & (RE_CMD_TXENB | RE_CMD_RXENB));

	/* Soft reset. */
	RE_WRITE_1(sc, RE_CMD, RE_CMD_RESET);

	for (i = 0; i < RE_TIMEOUT; i++) {
		DELAY(100);
		if (!(RE_READ_1(sc, RE_CMD) & RE_CMD_RESET))
			break;
	}

	if (i == RE_TIMEOUT) {
		dev_err(sc->dev, CE_WARN, "reset never completed!");
		return ETIMEDOUT;
	}

	return 0;
}

/**
 * @brief Do initial chip power-on and setup.
 *
 * Must be called with the driver lock held.
 */
int
re_chipinit(struct re_softc *sc)
{
	int error;

	RE_ASSERT_LOCKED(sc);

	if ((error = re_exit_oob(sc)) != 0)
		return error;
	re_set_phy_power(sc, 1);
	re_hw_init(sc);
	re_hw_reset(sc);

	return 0;
}

static void
re_set_phy_power(struct re_softc *sc, int on)
{
	int i;

	if (on) {
		RE_SETBIT_1(sc, RE_PMCH, 0xc0);

		re_write_phy(sc, 0, MII_CONTROL, MII_CONTROL_ANE);

		for (i = 0; i < RE_TIMEOUT; i++) {
			if ((re_read_phy_ocp(sc, 0xa420) & 0x0007) == 3)
				break;
			DELAY(1000);
		}
	} else {
		re_write_phy(sc, 0, MII_CONTROL, MII_CONTROL_ANE | MII_CONTROL_PWRDN);
		RE_CLRBIT_1(sc, RE_PMCH, 0x80);
		RE_CLRBIT_1(sc, RE_PPSW, 0x40);
	}
}

void
re_mac_config_mcu(struct re_softc *sc, enum re_mac_type type)
{
	uint16_t reg;
	int i, npages;

	if (type == MAC_R25) {
		for (npages = 0; npages < 3; npages++) {
			re_switch_mcu_ram_page(sc, npages);
			for (i = 0; i < rtl8125_mac_bps_len; i++) {
				if (npages == 0)
					re_write_mac_ocp(sc,
					    rtl8125_mac_bps[i].reg,
					    rtl8125_mac_bps[i].val);
				else if (npages == 1)
					re_write_mac_ocp(sc,
					    rtl8125_mac_bps[i].reg, 0);
				else {
					if (rtl8125_mac_bps[i].reg < 0xf9f8)
						re_write_mac_ocp(sc,
						    rtl8125_mac_bps[i].reg, 0);
				}
			}
			if (npages == 2) {
				re_write_mac_ocp(sc, 0xf9f8, 0x6486);
				re_write_mac_ocp(sc, 0xf9fa, 0x0b15);
				re_write_mac_ocp(sc, 0xf9fc, 0x090e);
				re_write_mac_ocp(sc, 0xf9fe, 0x1139);
			}
		}
		re_write_mac_ocp(sc, 0xfc26, 0x8000);
		re_write_mac_ocp(sc, 0xfc2a, 0x0540);
		re_write_mac_ocp(sc, 0xfc2e, 0x0a06);
		re_write_mac_ocp(sc, 0xfc30, 0x0eb8);
		re_write_mac_ocp(sc, 0xfc32, 0x3a5c);
		re_write_mac_ocp(sc, 0xfc34, 0x10a8);
		re_write_mac_ocp(sc, 0xfc40, 0x0d54);
		re_write_mac_ocp(sc, 0xfc42, 0x0e24);
		re_write_mac_ocp(sc, 0xfc48, 0x307a);
	} else if (type == MAC_R25B) {
		re_switch_mcu_ram_page(sc, 0);
		for (i = 0; i < rtl8125b_mac_bps_len; i++) {
			re_write_mac_ocp(sc, rtl8125b_mac_bps[i].reg,
			    rtl8125b_mac_bps[i].val);
		}
	} else if (type == MAC_R25D_1) {
		for (npages = 0; npages < 3; npages++) {
			re_switch_mcu_ram_page(sc, npages);

			re_write_mac_ocp(sc, 0xf800,
			    (npages == 0) ? 0xe002 : 0);
			re_write_mac_ocp(sc, 0xf802,
			    (npages == 0) ? 0xe006 : 0);
			re_write_mac_ocp(sc, 0xf804,
			    (npages == 0) ? 0x4166 : 0);
			re_write_mac_ocp(sc, 0xf806,
			    (npages == 0) ? 0x9cf6 : 0);
			re_write_mac_ocp(sc, 0xf808,
			    (npages == 0) ? 0xc002 : 0);
			re_write_mac_ocp(sc, 0xf80a,
			    (npages == 0) ? 0xb800 : 0);
			re_write_mac_ocp(sc, 0xf80c,
			    (npages == 0) ? 0x14a4 : 0);
			re_write_mac_ocp(sc, 0xf80e,
			    (npages == 0) ? 0xc102 : 0);
			re_write_mac_ocp(sc, 0xf810,
			    (npages == 0) ? 0xb900 : 0);

			for (reg = 0xf812; reg <= 0xf9f6; reg += 2)
				re_write_mac_ocp(sc, reg, 0);

			re_write_mac_ocp(sc, 0xf9f8,
			    (npages == 2) ? 0x6938 : 0);
			re_write_mac_ocp(sc, 0xf9fa,
			    (npages == 2) ? 0x0a18 : 0);
			re_write_mac_ocp(sc, 0xf9fc,
			    (npages == 2) ? 0x0217 : 0);
			re_write_mac_ocp(sc, 0xf9fe,
			    (npages == 2) ? 0x0d2a : 0);
		}
		re_write_mac_ocp(sc, 0xfc26, 0x8000);
		re_write_mac_ocp(sc, 0xfc28, 0x14a2);
		re_write_mac_ocp(sc, 0xfc48, 0x0001);
	} else if (type == MAC_R25D_2) {
		for (npages = 0; npages < 3; npages++) {
			re_switch_mcu_ram_page(sc, npages);

			for (i = 0; i < rtl8125d_2_mac_bps_len; i++) {
				if (npages == 0)
					re_write_mac_ocp(sc,
					    rtl8125d_2_mac_bps[i].reg,
					    rtl8125d_2_mac_bps[i].val);
				else
					re_write_mac_ocp(sc,
					    rtl8125d_2_mac_bps[i].reg, 0);
			}

			for (reg = 0xf884; reg <= 0xf9f6; reg += 2)
				re_write_mac_ocp(sc, reg, 0);

			re_write_mac_ocp(sc, 0xf9f8,
			    (npages == 2) ? 0x6938 : 0);
			re_write_mac_ocp(sc, 0xf9fa,
			    (npages == 2) ? 0x0a19: 0);
			re_write_mac_ocp(sc, 0xf9fc,
			    (npages == 2) ? 0x030e: 0);
			re_write_mac_ocp(sc, 0xf9fe,
			    (npages == 2) ? 0x0b2f: 0);
		}
		re_write_mac_ocp(sc, 0xfc26, 0x8000);
		re_write_mac_ocp(sc, 0xfc28, 0x2382);
		re_write_mac_ocp(sc, 0xfc48, 0x0001);
	}
}

void
re_mac_config_ext_mcu(struct re_softc *sc, enum re_mac_type type)
{
	const struct re_mac_bps *bps;
	uint64_t mcodever = 0;
	int i;

	/* Read microcode version. */
	re_switch_mcu_ram_page(sc, 2);
	sc->re_mcodever = 0;
	for (i = 0; i < 8; i += 2) {
		sc->re_mcodever <<= 16;
		sc->re_mcodever |= re_read_mac_ocp(sc, 0xf9f8 + i);
	}
	re_switch_mcu_ram_page(sc, 0);

	if (type == MAC_R26_1) {
		bps = &rtl8126_1_mac_bps;
		mcodever =
		    re_mcu_get_bin_version(rtl8126_1_mac_bps_vals, bps->count);
		if (sc->re_mcodever != mcodever) {
			/* Switch to page 0. */
			re_switch_mcu_ram_page(sc, 0);
			for (i = 0; i < bps->count; i++)
				re_write_mac_ocp(sc, bps->regs[i],
				    bps->vals[i]);
		}
		re_write_mac_ocp(sc, 0xfc26, 0x8000);
		re_write_mac_ocp(sc, 0xfc2c, 0x2360);
		re_write_mac_ocp(sc, 0xfc2e, 0x14a4);
		re_write_mac_ocp(sc, 0xfc30, 0x415e);
		re_write_mac_ocp(sc, 0xfc32, 0x41e4);
		re_write_mac_ocp(sc, 0xfc34, 0x4280);
		re_write_mac_ocp(sc, 0xfc36, 0x234a);
		re_write_mac_ocp(sc, 0xfc48, 0x00fc);
	} else if (type == MAC_R26_2) {
		bps = &rtl8126_2_mac_bps;
		mcodever =
		    re_mcu_get_bin_version(rtl8126_2_mac_bps_vals, bps->count);
		if (sc->re_mcodever != mcodever) {
			/* Switch to page 0. */
			re_switch_mcu_ram_page(sc, 0);
			for (i = 0; i < 256; i++)
				re_write_mac_ocp(sc, bps->regs[i],
				    bps->vals[i]);
			/* Switch to page 1. */
			re_switch_mcu_ram_page(sc, 1);
			for (; i < bps->count; i++)
				re_write_mac_ocp(sc, bps->regs[i],
				    bps->vals[i]);
		}
		re_write_mac_ocp(sc, 0xfc26, 0x8000);
		re_write_mac_ocp(sc, 0xfc2c, 0x14a4);
		re_write_mac_ocp(sc, 0xfc2e, 0x4176);
		re_write_mac_ocp(sc, 0xfc30, 0x41fc);
		re_write_mac_ocp(sc, 0xfc32, 0x4298);
		re_write_mac_ocp(sc, 0xfc3a, 0x234a);
		re_write_mac_ocp(sc, 0xfc48, 0x023c);
	} else if (type == MAC_R27) {
		bps = &rtl8127_mac_bps;
		mcodever =
		    re_mcu_get_bin_version(rtl8127_mac_bps_vals, bps->count);
		if (sc->re_mcodever != mcodever) {
				/* Switch to page 0. */
			re_switch_mcu_ram_page(sc, 0);
			for (i = 0; i < 256; i++)
				re_write_mac_ocp(sc, bps->regs[i],
				    bps->vals[i]);
				/* Switch to page 1. */
			re_switch_mcu_ram_page(sc, 1);
			for (; i < bps->count; i++)
				re_write_mac_ocp(sc, bps->regs[i],
				    bps->vals[i]);
		}
		re_write_mac_ocp(sc, 0xfc26, 0x8000);
		re_write_mac_ocp(sc, 0xfc28, 0x1520);
		re_write_mac_ocp(sc, 0xfc2a, 0x41e0);
		re_write_mac_ocp(sc, 0xfc2c, 0x508c);
		re_write_mac_ocp(sc, 0xfc2e, 0x50f6);
		re_write_mac_ocp(sc, 0xfc30, 0x34fa);
		re_write_mac_ocp(sc, 0xfc32, 0x0166);
		re_write_mac_ocp(sc, 0xfc34, 0x1a6a);
		re_write_mac_ocp(sc, 0xfc36, 0x1a2c);
		re_write_mac_ocp(sc, 0xfc48, 0x00ff);
	}

	/* Write microcode version. */
	re_mcu_set_version(sc, mcodever);
};

static uint64_t
re_mcu_get_bin_version(const uint16_t *mac_bps, uint16_t entries)
{
	uint64_t binver = 0;
	int i;

	for (i = 0; i < 4; i++) {
		binver <<= 16;
		binver |= mac_bps[entries - 4 + i];
	}

	return binver;
}

static void
re_mcu_set_version(struct re_softc *sc, uint64_t mcodever)
{
	int i;

	/* Switch to page 2. */
	re_switch_mcu_ram_page(sc, 2);

	for (i = 0; i < 8; i += 2) {
		re_write_mac_ocp(sc, 0xf9f8 + 6 - i, (uint16_t)mcodever);
		mcodever >>= 16;
	}

	/* Switch back to page 0. */
	re_switch_mcu_ram_page(sc, 0);
}

void
re_ephy_config(struct re_softc *sc)
{
	switch (sc->re_type) {
	case MAC_R25:
		re_ephy_config_mac_r25(sc);
		break;
	case MAC_R25B:
		re_ephy_config_mac_r25b(sc);
		break;
	case MAC_R27:
		re_ephy_config_mac_r27(sc);
		break;
	default:
		break;	/* Nothing to do. */
	}
}

static void
re_ephy_config_mac_r25(struct re_softc *sc)
{
	uint16_t val;
	int i;

	for (i = 0; i < mac_r25_ephy_len; i++)
		re_write_ephy(sc, mac_r25_ephy[i].reg, mac_r25_ephy[i].val);

	val = re_read_ephy(sc, 0x002a) & ~0x7000;
	re_write_ephy(sc, 0x002a, val | 0x3000);
	RE_EPHY_CLRBIT(sc, 0x0019, 0x0040);
	RE_EPHY_SETBIT(sc, 0x001b, 0x0e00);
	RE_EPHY_CLRBIT(sc, 0x001b, 0x7000);
	re_write_ephy(sc, 0x0002, 0x6042);
	re_write_ephy(sc, 0x0006, 0x0014);
	val = re_read_ephy(sc, 0x006a) & ~0x7000;
	re_write_ephy(sc, 0x006a, val | 0x3000);
	RE_EPHY_CLRBIT(sc, 0x0059, 0x0040);
	RE_EPHY_SETBIT(sc, 0x005b, 0x0e00);
	RE_EPHY_CLRBIT(sc, 0x005b, 0x7000);
	re_write_ephy(sc, 0x0042, 0x6042);
	re_write_ephy(sc, 0x0046, 0x0014);
}

static void
re_ephy_config_mac_r25b(struct re_softc *sc)
{
	int i;

	for (i = 0; i < mac_r25b_ephy_len; i++)
		re_write_ephy(sc, mac_r25b_ephy[i].reg, mac_r25b_ephy[i].val);
}

static void
re_ephy_config_mac_r27(struct re_softc *sc)
{
	int i;

	for (i = 0; i < mac_r27_ephy_len; i++)
		re_r27_write_ephy(sc, mac_r27_ephy[i].reg,
		    mac_r27_ephy[i].val);

	/* Clear extended address. */
	re_write_ephy(sc, RE_EPHYAR_EXT_ADDR, 0);
}

int
re_phy_config(struct re_softc *sc)
{
	uint16_t val = 0;
	int i;

	re_ephy_config(sc);

	/* PHY reset. */
	re_write_phy(sc, 0, MII_AN_ADVERT,
	    re_read_phy(sc, 0, MII_AN_ADVERT) &
	    ~(MII_ABILITY_100BASE_TX_FD | MII_ABILITY_100BASE_TX | MII_ABILITY_10BASE_T_FD | MII_ABILITY_10BASE_T));
	re_write_phy(sc, 0, MII_MSCONTROL,
	    re_read_phy(sc, 0, MII_MSCONTROL) &
	    ~(MII_MSCONTROL_1000T_FD | MII_MSCONTROL_1000T));
	switch (sc->re_type) {
	case MAC_R27:
		val |= RE_ADV_10000TFDX;
		/* fallthrough */
	case MAC_R26_1:
	case MAC_R26_2:
		val |= RE_ADV_5000TFDX;
		/* fallthrough */
	default:
		val |= RE_ADV_2500TFDX;
		break;
	}
	RE_PHY_CLRBIT(sc, 0xa5d4, val);
	re_write_phy(sc, 0, MII_CONTROL, MII_CONTROL_RESET | MII_CONTROL_ANE |
	    MII_CONTROL_RSAN);
	for (i = 0; i < 2500; i++) {
		if (!(re_read_phy(sc, 0, MII_CONTROL) & MII_CONTROL_RESET))
			break;
		DELAY(1000);
	}
	if (i == 2500) {
		dev_err(sc->dev, CE_WARN, "PHY reset failed");
		return (ETIMEDOUT);
	}

	/* Read ram code version. */
	re_write_phy_ocp(sc, 0xa436, 0x801e);
	sc->re_rcodever = re_read_phy_ocp(sc, 0xa438);

	switch (sc->re_type) {
	case MAC_R25:
		re_phy_config_mac_r25(sc);
		break;
	case MAC_R25B:
		re_phy_config_mac_r25b(sc);
		break;
	case MAC_R25D_1:
		re_phy_config_mac_r25d_1(sc);
		break;
	case MAC_R25D_2:
		re_phy_config_mac_r25d_2(sc);
		break;
	case MAC_R26_1:
		re_phy_config_mac_r26_1(sc);
		break;
	case MAC_R26_2:
		re_phy_config_mac_r26_2(sc);
		break;
	case MAC_R27:
		re_phy_config_mac_r27(sc);
		break;
	default:
		break;	/* Can't happen. */
	}

	RE_PHY_CLRBIT(sc, 0xa5b4, 0x8000);

	/* Disable EEE. */
	RE_MAC_CLRBIT(sc, 0xe040, 0x0003);
	if (sc->re_type == MAC_R25) {
		RE_MAC_CLRBIT(sc, 0xeb62, 0x0006);
		RE_PHY_CLRBIT(sc, 0xa432, 0x0010);
	} else if (sc->re_type == MAC_R25B || RE_TYPE_R25D(sc))
		RE_PHY_SETBIT(sc, 0xa432, 0x0010);

	RE_PHY_CLRBIT(sc, 0xa5d0, (sc->re_type == MAC_R27) ? 0x000e : 0x0006);
	RE_PHY_CLRBIT(sc, 0xa6d4, 0x0001);
	if (RE_TYPE_R26(sc) || sc->re_type == MAC_R27)
		RE_PHY_CLRBIT(sc, 0xa6d4, 0x0002);
	RE_PHY_CLRBIT(sc, 0xa6d8, 0x0010);
	RE_PHY_CLRBIT(sc, 0xa428, 0x0080);
	RE_PHY_CLRBIT(sc, 0xa4a2, 0x0200);

	/* Disable advanced EEE. */
	RE_MAC_CLRBIT(sc, 0xe052, 0x0001);
	RE_PHY_CLRBIT(sc, 0xa442, 0x3000);
	RE_PHY_CLRBIT(sc, 0xa430, 0x8000);

	return (0);
}

static void
re_phy_config_mac_r27(struct re_softc *sc)
{
	uint16_t val;
	int i;
	static const uint16_t mac_cfg_value[] =
	    { 0x815a, 0x0150, 0x81f4, 0x0150, 0x828e, 0x0150, 0x81b1, 0x0000,
	      0x824b, 0x0000, 0x82e5, 0x0000 };

	static const uint16_t mac_cfg2_value[] =
	    { 0x88d7, 0x01a0, 0x88d9, 0x01a0, 0x8ffa, 0x002a, 0x8fee, 0xffdf,
	      0x8ff0, 0xffff, 0x8ff2, 0x0a4a, 0x8ff4, 0xaa5a, 0x8ff6, 0x0a4a,
	      0x8ff8, 0xaa5a };

	static const uint16_t mac_cfg_a438_value[] =
	    { 0x003b, 0x0086, 0x00b7, 0x00db, 0x00fe, 0x00fe, 0x00fe, 0x00fe,
	      0x00c3, 0x0078, 0x0047, 0x0023 };

	re_phy_config_mcu(sc, RE_MAC_R27_RCODE_VER);

	re_write_phy_ocp(sc, 0xa4d2, 0x0000);
	(void) re_read_phy_ocp(sc, 0xa4d4);

	RE_PHY_CLRBIT(sc, 0xa442, 0x0800);
	re_write_phy_ocp(sc, 0xa436, 0x8415);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x9300);
	re_write_phy_ocp(sc, 0xa436, 0x81a3);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0f00);
	re_write_phy_ocp(sc, 0xa436, 0x81ae);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0f00);
	re_write_phy_ocp(sc, 0xa436, 0x81b9);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xb900);
	re_write_phy_ocp(sc, 0xb87c, 0x83b0);
	RE_PHY_CLRBIT(sc,0xb87e, 0x0e00);
	re_write_phy_ocp(sc, 0xb87c, 0x83c5);
	RE_PHY_CLRBIT(sc, 0xb87e, 0x0e00);
	re_write_phy_ocp(sc, 0xb87c, 0x83da);
	RE_PHY_CLRBIT(sc, 0xb87e, 0x0e00);
	re_write_phy_ocp(sc, 0xb87c, 0x83ef);
	RE_PHY_CLRBIT(sc, 0xb87e, 0x0e00);
	val = re_read_phy_ocp(sc, 0xbf38) & ~0x01f0;
	re_write_phy_ocp(sc, 0xbf38, val | 0x0160);
	val = re_read_phy_ocp(sc, 0xbf3a) & ~0x001f;
	re_write_phy_ocp(sc, 0xbf3a, val | 0x0014);
	RE_PHY_CLRBIT(sc, 0xbf28, 0x6000);
	RE_PHY_CLRBIT(sc, 0xbf2c, 0xc000);
	val = re_read_phy_ocp(sc, 0xbf28) & ~0x1fff;
	re_write_phy_ocp(sc, 0xbf28, val | 0x0187);
	val = re_read_phy_ocp(sc, 0xbf2a) & ~0x003f;
	re_write_phy_ocp(sc, 0xbf2a, val | 0x0003);
	re_write_phy_ocp(sc, 0xa436, 0x8173);
	re_write_phy_ocp(sc, 0xa438, 0x8620);
	re_write_phy_ocp(sc, 0xa436, 0x8175);
	re_write_phy_ocp(sc, 0xa438, 0x8671);
	re_write_phy_ocp(sc, 0xa436, 0x817c);
	RE_PHY_SETBIT(sc, 0xa438, 0x2000);
	re_write_phy_ocp(sc, 0xa436, 0x8187);
	RE_PHY_SETBIT(sc, 0xa438, 0x2000);
	re_write_phy_ocp(sc, 0xA436, 0x8192);
	RE_PHY_SETBIT(sc, 0xA438, 0x2000);
	re_write_phy_ocp(sc, 0xA436, 0x819D);
	RE_PHY_SETBIT(sc, 0xA438, 0x2000);
	re_write_phy_ocp(sc, 0xA436, 0x81A8);
	RE_PHY_CLRBIT(sc, 0xA438, 0x2000);
	re_write_phy_ocp(sc, 0xA436, 0x81B3);
	RE_PHY_CLRBIT(sc, 0xA438, 0x2000);
	re_write_phy_ocp(sc, 0xA436, 0x81BE);
	RE_PHY_SETBIT(sc, 0xA438, 0x2000);
	re_write_phy_ocp(sc, 0xa436, 0x817d);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xa600);
	re_write_phy_ocp(sc, 0xa436, 0x8188);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xa600);
	re_write_phy_ocp(sc, 0xa436, 0x8193);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xa600);
	re_write_phy_ocp(sc, 0xa436, 0x819e);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xa600);
	re_write_phy_ocp(sc, 0xa436, 0x81a9);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x1400);
	re_write_phy_ocp(sc, 0xa436, 0x81b4);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x1400);
	re_write_phy_ocp(sc, 0xa436, 0x81bf);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xa600);
	RE_PHY_CLRBIT(sc, 0xaeaa, 0x0028);
	re_write_phy_ocp(sc, 0xb87c, 0x84f0);
	re_write_phy_ocp(sc, 0xb87e, 0x201c);
	re_write_phy_ocp(sc, 0xb87c, 0x84f2);
	re_write_phy_ocp(sc, 0xb87e, 0x3117);
	re_write_phy_ocp(sc, 0xaec6, 0x0000);
	re_write_phy_ocp(sc, 0xae20, 0xffff);
	re_write_phy_ocp(sc, 0xaece, 0xffff);
	re_write_phy_ocp(sc, 0xaed2, 0xffff);
	re_write_phy_ocp(sc, 0xaec8, 0x0000);
	RE_PHY_CLRBIT(sc, 0xaed0, 0x0001);
	re_write_phy_ocp(sc, 0xadb8, 0x0150);
	re_write_phy_ocp(sc, 0xb87c, 0x8197);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5000);
	re_write_phy_ocp(sc, 0xb87c, 0x8231);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5000);
	re_write_phy_ocp(sc, 0xb87c, 0x82cb);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5000);
	re_write_phy_ocp(sc, 0xb87c, 0x82cd);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5700);
	re_write_phy_ocp(sc, 0xb87c, 0x8233);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5700);
	re_write_phy_ocp(sc, 0xb87c, 0x8199);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5700);
	for (i = 0; i < nitems(mac_cfg_value); i+=2) {
		re_write_phy_ocp(sc, 0xb87c, mac_cfg_value[i]);
		re_write_phy_ocp(sc, 0xb87e, mac_cfg_value[i + 1]);
	}
	re_write_phy_ocp(sc, 0xb87c, 0x84f7);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x2800);
	RE_PHY_SETBIT(sc, 0xaec2, 0x1000);
	re_write_phy_ocp(sc, 0xb87c, 0x81b3);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xad00);
	re_write_phy_ocp(sc, 0xb87c, 0x824d);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xad00);
	re_write_phy_ocp(sc, 0xb87c, 0x82e7);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xad00);
	val = re_read_phy_ocp(sc, 0xae4e) & ~0x000f;
	re_write_phy_ocp(sc, 0xae4e, val | 0x0001);
	re_write_phy_ocp(sc, 0xb87c, 0x82ce);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xf000;
	re_write_phy_ocp(sc, 0xb87e, val | 0x4000);
	re_write_phy_ocp(sc, 0xb87c, 0x84ac);
	re_write_phy_ocp(sc, 0xb87e, 0x0000);
	re_write_phy_ocp(sc, 0xb87c, 0x84ae);
	re_write_phy_ocp(sc, 0xb87e, 0x0000);
	re_write_phy_ocp(sc, 0xb87c, 0x84b0);
	re_write_phy_ocp(sc, 0xb87e, 0xf818);
	re_write_phy_ocp(sc, 0xb87c, 0x84b2);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x6000);
	re_write_phy_ocp(sc, 0xb87c, 0x8ffc);
	re_write_phy_ocp(sc, 0xb87e, 0x6008);
	re_write_phy_ocp(sc, 0xb87c, 0x8ffe);
	re_write_phy_ocp(sc, 0xb87e, 0xf450);
	re_write_phy_ocp(sc, 0xb87c, 0x8015);
	RE_PHY_SETBIT(sc, 0xb87e, 0x0200);
	re_write_phy_ocp(sc, 0xb87c, 0x8016);
	RE_PHY_CLRBIT(sc, 0xb87e, 0x0800);
	re_write_phy_ocp(sc, 0xb87c, 0x8fe6);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0800);
	re_write_phy_ocp(sc, 0xb87c, 0x8fe4);
	re_write_phy_ocp(sc, 0xb87e, 0x2114);
	re_write_phy_ocp(sc, 0xb87c, 0x8647);
	re_write_phy_ocp(sc, 0xb87e, 0xa7B1);
	re_write_phy_ocp(sc, 0xb87c, 0x8649);
	re_write_phy_ocp(sc, 0xb87e, 0xbbca);
	re_write_phy_ocp(sc, 0xb87c, 0x864b);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xdc00);
	re_write_phy_ocp(sc, 0xb87c, 0x8154);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xc000;
	re_write_phy_ocp(sc, 0xb87e, val | 0x4000);
	re_write_phy_ocp(sc, 0xb87c, 0x8158);
	RE_PHY_CLRBIT(sc, 0xb87e, 0xc000);
	re_write_phy_ocp(sc, 0xb87c, 0x826c);
	re_write_phy_ocp(sc, 0xb87e, 0xffff);
	re_write_phy_ocp(sc, 0xb87c, 0x826e);
	re_write_phy_ocp(sc, 0xb87e, 0xffff);
	re_write_phy_ocp(sc, 0xb87c, 0x8872);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0e00);
	re_write_phy_ocp(sc, 0xa436, 0x8012);
	RE_PHY_SETBIT(sc, 0xa438, 0x0800);
	re_write_phy_ocp(sc, 0xa436, 0x8012);
	RE_PHY_SETBIT(sc, 0xa438, 0x4000);
	RE_PHY_SETBIT(sc, 0xb576, 0x0001);
	re_write_phy_ocp(sc, 0xa436, 0x834a);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0700);
	re_write_phy_ocp(sc, 0xb87c, 0x8217);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0x3f00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x2a00);
	re_write_phy_ocp(sc, 0xa436, 0x81b1);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0b00);
	re_write_phy_ocp(sc, 0xb87c, 0x8fed);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x4e00);
	re_write_phy_ocp(sc, 0xb87c, 0x88ac);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x2300);
	RE_PHY_SETBIT(sc, 0xbf0c, 0x3800);
	re_write_phy_ocp(sc, 0xb87c, 0x88de);
	RE_PHY_CLRBIT(sc, 0xb87e, 0xFF00);
	re_write_phy_ocp(sc, 0xb87c, 0x80B4);
	re_write_phy_ocp(sc, 0xb87e, 0x5195);
	re_write_phy_ocp(sc, 0xa436, 0x8370);
	re_write_phy_ocp(sc, 0xa438, 0x8671);
	re_write_phy_ocp(sc, 0xa436, 0x8372);
	re_write_phy_ocp(sc, 0xa438, 0x86c8);
	re_write_phy_ocp(sc, 0xa436, 0x8401);
	re_write_phy_ocp(sc, 0xa438, 0x86c8);
	re_write_phy_ocp(sc, 0xa436, 0x8403);
	re_write_phy_ocp(sc, 0xa438, 0x86da);
	re_write_phy_ocp(sc, 0xa436, 0x8406);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x8408);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x840a);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x840c);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x840e);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x8410);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x8412);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x8414);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x8416);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x1800;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x82bd);
	re_write_phy_ocp(sc, 0xa438, 0x1f40);
	val = re_read_phy_ocp(sc, 0xbfb4) & ~0x07ff;
	re_write_phy_ocp(sc, 0xbfb4, val | 0x0328);
	re_write_phy_ocp(sc, 0xbfb6, 0x3e14);
	re_write_phy_ocp(sc, 0xa436, 0x81c4);
	for (i = 0; i < nitems(mac_cfg_a438_value); i++)
		re_write_phy_ocp(sc, 0xa438, mac_cfg_a438_value[i]);
	for (i = 0; i < nitems(mac_cfg2_value); i+=2) {
		re_write_phy_ocp(sc, 0xb87c, mac_cfg2_value[i]);
		re_write_phy_ocp(sc, 0xb87e, mac_cfg2_value[i + 1]);
	}
	re_write_phy_ocp(sc, 0xb87c, 0x88d5);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0200);
	re_write_phy_ocp(sc, 0xa436, 0x84bb);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0a00);
	re_write_phy_ocp(sc, 0xa436, 0x84c0);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x1600);
	RE_PHY_SETBIT(sc, 0xa430, 0x0003);
}

static void
re_phy_config_mac_r26_1(struct re_softc *sc)
{
	uint16_t val;
	int i;
	static const uint16_t mac_cfg2_a438_value[] =
	    { 0x0044, 0x00a8, 0x00d6, 0x00ec, 0x00f6, 0x00fc, 0x00fe,
	      0x00fe, 0x00bc, 0x0058, 0x002a, 0x003f, 0x3f02, 0x023c,
	      0x3b0a, 0x1c00, 0x0000, 0x0000, 0x0000, 0x0000 };

	static const uint16_t mac_cfg2_b87e_value[] =
	    { 0x03ed, 0x03ff, 0x0009, 0x03fe, 0x000b, 0x0021, 0x03f7,
	      0x03b8, 0x03e0, 0x0049, 0x0049, 0x03e0, 0x03b8, 0x03f7,
	      0x0021, 0x000b, 0x03fe, 0x0009, 0x03ff, 0x03ed, 0x000e,
	      0x03fe, 0x03ed, 0x0006, 0x001a, 0x03f1, 0x03d8, 0x0023,
	      0x0054, 0x0322, 0x00dd, 0x03ab, 0x03dc, 0x0027, 0x000e,
	      0x03e5, 0x03f9, 0x0012, 0x0001, 0x03f1 };

	re_phy_config_mcu(sc, RE_MAC_R26_1_RCODE_VER);

	RE_PHY_SETBIT(sc, 0xa442, 0x0800);
	re_write_phy_ocp(sc, 0xa436, 0x80bf);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xed00);
	re_write_phy_ocp(sc, 0xa436, 0x80cd);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x1000);
	re_write_phy_ocp(sc, 0xa436, 0x80d1);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xc800);
	re_write_phy_ocp(sc, 0xa436, 0x80d4);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xc800);
	re_write_phy_ocp(sc, 0xa436, 0x80e1);
	re_write_phy_ocp(sc, 0xa438, 0x10cc);
	re_write_phy_ocp(sc, 0xa436, 0x80e5);
	re_write_phy_ocp(sc, 0xa438, 0x4f0c);
	re_write_phy_ocp(sc, 0xa436, 0x8387);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x4700);
	val = re_read_phy_ocp(sc, 0xa80c) & ~0x00c0;
	re_write_phy_ocp(sc, 0xa80c, val | 0x0080);
	RE_PHY_CLRBIT(sc, 0xac90, 0x0010);
	RE_PHY_CLRBIT(sc, 0xad2c, 0x8000);
	re_write_phy_ocp(sc, 0xb87c, 0x8321);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x1100);
	RE_PHY_SETBIT(sc, 0xacf8, 0x000c);
	re_write_phy_ocp(sc, 0xa436, 0x8183);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x5900);
	RE_PHY_SETBIT(sc, 0xad94, 0x0020);
	RE_PHY_CLRBIT(sc, 0xa654, 0x0800);
	RE_PHY_SETBIT(sc, 0xb648, 0x4000);
	re_write_phy_ocp(sc, 0xb87c, 0x839e);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x2f00);
	re_write_phy_ocp(sc, 0xb87c, 0x83f2);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0800);
	RE_PHY_SETBIT(sc, 0xada0, 0x0002);
	re_write_phy_ocp(sc, 0xb87c, 0x80f3);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x9900);
	re_write_phy_ocp(sc, 0xb87c, 0x8126);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xc100);
	re_write_phy_ocp(sc, 0xb87c, 0x893a);
	re_write_phy_ocp(sc, 0xb87e, 0x8080);
	re_write_phy_ocp(sc, 0xb87c, 0x8647);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xe600);
	re_write_phy_ocp(sc, 0xb87c, 0x862c);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x1200);
	re_write_phy_ocp(sc, 0xb87c, 0x864a);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xe600);
	re_write_phy_ocp(sc, 0xb87c, 0x80a0);
	re_write_phy_ocp(sc, 0xb87e, 0xbcbc);
	re_write_phy_ocp(sc, 0xb87c, 0x805e);
	re_write_phy_ocp(sc, 0xb87e, 0xbcbc);
	re_write_phy_ocp(sc, 0xb87c, 0x8056);
	re_write_phy_ocp(sc, 0xb87e, 0x3077);
	re_write_phy_ocp(sc, 0xb87c, 0x8058);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5a00);
	re_write_phy_ocp(sc, 0xb87c, 0x8098);
	re_write_phy_ocp(sc, 0xb87e, 0x3077);
	re_write_phy_ocp(sc, 0xb87c, 0x809a);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5a00);
	re_write_phy_ocp(sc, 0xb87c, 0x8052);
	re_write_phy_ocp(sc, 0xb87e, 0x3733);
	re_write_phy_ocp(sc, 0xb87c, 0x8094);
	re_write_phy_ocp(sc, 0xb87e, 0x3733);
	re_write_phy_ocp(sc, 0xb87c, 0x807f);
	re_write_phy_ocp(sc, 0xb87e, 0x7c75);
	re_write_phy_ocp(sc, 0xb87c, 0x803d);
	re_write_phy_ocp(sc, 0xb87e, 0x7c75);
	re_write_phy_ocp(sc, 0xb87c, 0x8036);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3000);
	re_write_phy_ocp(sc, 0xb87c, 0x8078);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3000);
	re_write_phy_ocp(sc, 0xb87c, 0x8031);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3300);
	re_write_phy_ocp(sc, 0xb87c, 0x8073);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3300);
	val = re_read_phy_ocp(sc, 0xae06) & ~0xfc00;
	re_write_phy_ocp(sc, 0xae06, val | 0x7c00);
	re_write_phy_ocp(sc, 0xb87c, 0x89D1);
	re_write_phy_ocp(sc, 0xb87e, 0x0004);
	re_write_phy_ocp(sc, 0xa436, 0x8fbd);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0a00);
	re_write_phy_ocp(sc, 0xa436, 0x8fbe);
	re_write_phy_ocp(sc, 0xa438, 0x0d09);
	re_write_phy_ocp(sc, 0xb87c, 0x89cd);
	re_write_phy_ocp(sc, 0xb87e, 0x0f0f);
	re_write_phy_ocp(sc, 0xb87c, 0x89cf);
	re_write_phy_ocp(sc, 0xb87e, 0x0f0f);
	re_write_phy_ocp(sc, 0xb87c, 0x83a4);
	re_write_phy_ocp(sc, 0xb87e, 0x6600);
	re_write_phy_ocp(sc, 0xb87c, 0x83a6);
	re_write_phy_ocp(sc, 0xb87e, 0x6601);
	re_write_phy_ocp(sc, 0xb87c, 0x83c0);
	re_write_phy_ocp(sc, 0xb87e, 0x6600);
	re_write_phy_ocp(sc, 0xb87c, 0x83c2);
	re_write_phy_ocp(sc, 0xb87e, 0x6601);
	re_write_phy_ocp(sc, 0xb87c, 0x8414);
	re_write_phy_ocp(sc, 0xb87e, 0x6600);
	re_write_phy_ocp(sc, 0xb87c, 0x8416);
	re_write_phy_ocp(sc, 0xb87e, 0x6601);
	re_write_phy_ocp(sc, 0xb87c, 0x83f8);
	re_write_phy_ocp(sc, 0xb87e, 0x6600);
	re_write_phy_ocp(sc, 0xb87c, 0x83fa);
	re_write_phy_ocp(sc, 0xb87e, 0x6601);

	re_patch_phy_mcu(sc, 1);
	val = re_read_phy_ocp(sc, 0xbd96) & ~0x1f00;
	re_write_phy_ocp(sc, 0xbd96, val | 0x1000);
	val = re_read_phy_ocp(sc, 0xbf1c) & ~0x0007;
	re_write_phy_ocp(sc, 0xbf1c, val | 0x0007);
	RE_PHY_CLRBIT(sc, 0xbfbe, 0x8000);
	val = re_read_phy_ocp(sc, 0xbf40) & ~0x0380;
	re_write_phy_ocp(sc, 0xbf40, val | 0x0280);
	val = re_read_phy_ocp(sc, 0xbf90) & ~0x0080;
	re_write_phy_ocp(sc, 0xbf90, val | 0x0060);
	val = re_read_phy_ocp(sc, 0xbf90) & ~0x0010;
	re_write_phy_ocp(sc, 0xbf90, val | 0x000c);
	re_patch_phy_mcu(sc, 0);

	re_write_phy_ocp(sc, 0xa436, 0x843b);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x2000);
	re_write_phy_ocp(sc, 0xa436, 0x843d);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x2000);
	RE_PHY_CLRBIT(sc, 0xb516, 0x007f);
	RE_PHY_CLRBIT(sc, 0xbf80, 0x0030);

	re_write_phy_ocp(sc, 0xa436, 0x8188);
	for (i = 0; i < 11; i++)
		re_write_phy_ocp(sc, 0xa438, mac_cfg2_a438_value[i]);

	re_write_phy_ocp(sc, 0xb87c, 0x8015);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0800);
	re_write_phy_ocp(sc, 0xb87c, 0x8ffd);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0);
	re_write_phy_ocp(sc, 0xb87c, 0x8fff);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x7f00);
	re_write_phy_ocp(sc, 0xb87c, 0x8ffb);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8fe9);
	re_write_phy_ocp(sc, 0xb87e, 0x0002);
	re_write_phy_ocp(sc, 0xb87c, 0x8fef);
	re_write_phy_ocp(sc, 0xb87e, 0x00a5);
	re_write_phy_ocp(sc, 0xb87c, 0x8ff1);
	re_write_phy_ocp(sc, 0xb87e, 0x0106);
	re_write_phy_ocp(sc, 0xb87c, 0x8fe1);
	re_write_phy_ocp(sc, 0xb87e, 0x0102);
	re_write_phy_ocp(sc, 0xb87c, 0x8fe3);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0400);
	RE_PHY_SETBIT(sc, 0xa654, 0x0800);
	RE_PHY_CLRBIT(sc, 0xa654, 0x0003);
	re_write_phy_ocp(sc, 0xac3a, 0x5851);
	val = re_read_phy_ocp(sc, 0xac3c) & ~0xd000;
	re_write_phy_ocp(sc, 0xac3c, val | 0x2000);
	val = re_read_phy_ocp(sc, 0xac42) & ~0x0200;
	re_write_phy_ocp(sc, 0xac42, val | 0x01c0);
	RE_PHY_CLRBIT(sc, 0xac3e, 0xe000);
	RE_PHY_CLRBIT(sc, 0xac42, 0x0038);
	val = re_read_phy_ocp(sc, 0xac42) & ~0x0002;
	re_write_phy_ocp(sc, 0xac42, val | 0x0005);
	re_write_phy_ocp(sc, 0xac1a, 0x00db);
	re_write_phy_ocp(sc, 0xade4, 0x01b5);
	RE_PHY_CLRBIT(sc, 0xad9c, 0x0c00);
	re_write_phy_ocp(sc, 0xb87c, 0x814b);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x1100);
	re_write_phy_ocp(sc, 0xb87c, 0x814d);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x1100);
	re_write_phy_ocp(sc, 0xb87c, 0x814f);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0b00);
	re_write_phy_ocp(sc, 0xb87c, 0x8142);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8144);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8150);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8118);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0700);
	re_write_phy_ocp(sc, 0xb87c, 0x811a);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0700);
	re_write_phy_ocp(sc, 0xb87c, 0x811c);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0500);
	re_write_phy_ocp(sc, 0xb87c, 0x810f);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8111);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x811d);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	RE_PHY_SETBIT(sc, 0xac36, 0x1000);
	RE_PHY_CLRBIT(sc, 0xad1c, 0x0100);
	val = re_read_phy_ocp(sc, 0xade8) & ~0xffc0;
	re_write_phy_ocp(sc, 0xade8, val | 0x1400);
	re_write_phy_ocp(sc, 0xb87c, 0x864b);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x9d00);

	re_write_phy_ocp(sc, 0xa436, 0x8f97);
	for (; i < nitems(mac_cfg2_a438_value); i++)
		re_write_phy_ocp(sc, 0xa438, mac_cfg2_a438_value[i]);

	RE_PHY_SETBIT(sc, 0xad9c, 0x0020);
	re_write_phy_ocp(sc, 0xb87c, 0x8122);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0c00);

	re_write_phy_ocp(sc, 0xb87c, 0x82c8);
	for (i = 0; i < 20; i++)
		re_write_phy_ocp(sc, 0xb87e, mac_cfg2_b87e_value[i]);

	re_write_phy_ocp(sc, 0xb87c, 0x80ef);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0c00);

	re_write_phy_ocp(sc, 0xb87c, 0x82a0);
	for (; i < nitems(mac_cfg2_b87e_value); i++)
		re_write_phy_ocp(sc, 0xb87e, mac_cfg2_b87e_value[i]);

	re_write_phy_ocp(sc, 0xa436, 0x8018);
	RE_PHY_SETBIT(sc, 0xa438, 0x2000);
	re_write_phy_ocp(sc, 0xb87c, 0x8fe4);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0);
	val = re_read_phy_ocp(sc, 0xb54c) & ~0xffc0;
	re_write_phy_ocp(sc, 0xb54c, val | 0x3700);
}

static void
re_phy_config_mac_r26_2(struct re_softc *sc)
{
	uint16_t val;
	int i;
	static const uint16_t mac_cfg3_b87e_value[] =
	    { 0x03ed, 0x03ff, 0x0009, 0x03fe, 0x000b, 0x0021, 0x03f7,
	      0x03b8, 0x03e0, 0x0049, 0x0049, 0x03e0, 0x03b8, 0x03f7,
	      0x0021, 0x000b, 0x03fe, 0x0009, 0x03ff, 0x03ed, 0x82a0,
	      0x000e, 0x03fe, 0x03ed, 0x0006, 0x001a, 0x03f1, 0x03d8,
	      0x0023, 0x0054, 0x0322, 0x00dd, 0x03ab, 0x03dc, 0x0027,
	      0x000e, 0x03e5, 0x03f9, 0x0012, 0x0001, 0x03f1 };

	re_phy_config_mcu(sc, RE_MAC_R26_2_RCODE_VER);

	RE_PHY_SETBIT(sc, 0xa442, 0x0800);
	re_write_phy_ocp(sc, 0xa436, 0x8183);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x5900);
	RE_PHY_SETBIT(sc, 0xa654, 0x0800);
	RE_PHY_SETBIT(sc, 0xb648, 0x4000);
	RE_PHY_SETBIT(sc, 0xad2c, 0x8000);
	RE_PHY_SETBIT(sc, 0xad94, 0x0020);
	RE_PHY_SETBIT(sc, 0xada0, 0x0002);
	val = re_read_phy_ocp(sc, 0xae06) & ~0xfc00;
	re_write_phy_ocp(sc, 0xae06, val | 0x7c00);
	re_write_phy_ocp(sc, 0xb87c, 0x8647);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xe600);
	re_write_phy_ocp(sc, 0xb87c, 0x8036);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3000);
	re_write_phy_ocp(sc, 0xb87c, 0x8078);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3000);
	re_write_phy_ocp(sc, 0xb87c, 0x89e9);
	RE_PHY_SETBIT(sc, 0xb87e, 0xff00);
	re_write_phy_ocp(sc, 0xb87c, 0x8ffd);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8ffe);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0200);
	re_write_phy_ocp(sc, 0xb87c, 0x8fff);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0400);
	re_write_phy_ocp(sc, 0xa436, 0x8018);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x7700);
	re_write_phy_ocp(sc, 0xa436, 0x8f9c);
	re_write_phy_ocp(sc, 0xa438, 0x0005);
	re_write_phy_ocp(sc, 0xa438, 0x0000);
	re_write_phy_ocp(sc, 0xa438, 0x00ed);
	re_write_phy_ocp(sc, 0xa438, 0x0502);
	re_write_phy_ocp(sc, 0xa438, 0x0b00);
	re_write_phy_ocp(sc, 0xa438, 0xd401);
	re_write_phy_ocp(sc, 0xa436, 0x8fa8);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x2900);
	re_write_phy_ocp(sc, 0xb87c, 0x814b);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x1100);
	re_write_phy_ocp(sc, 0xb87c, 0x814d);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x1100);
	re_write_phy_ocp(sc, 0xb87c, 0x814f);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0b00);
	re_write_phy_ocp(sc, 0xb87c, 0x8142);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8144);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8150);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8118);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0700);
	re_write_phy_ocp(sc, 0xb87c, 0x811a);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0700);
	re_write_phy_ocp(sc, 0xb87c, 0x811c);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0500);
	re_write_phy_ocp(sc, 0xb87c, 0x810f);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8111);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x811d);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	RE_PHY_SETBIT(sc, 0xad1c, 0x0100);
	val = re_read_phy_ocp(sc, 0xade8) & ~0xffc0;
	re_write_phy_ocp(sc, 0xade8, val | 0x1400);
	re_write_phy_ocp(sc, 0xb87c, 0x864b);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x9d00);
	re_write_phy_ocp(sc, 0xb87c, 0x862c);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x1200);
	re_write_phy_ocp(sc, 0xa436, 0x8566);
	re_write_phy_ocp(sc, 0xa438, 0x003f);
	re_write_phy_ocp(sc, 0xa438, 0x3f02);
	re_write_phy_ocp(sc, 0xa438, 0x023c);
	re_write_phy_ocp(sc, 0xa438, 0x3b0a);
	re_write_phy_ocp(sc, 0xa438, 0x1c00);
	re_write_phy_ocp(sc, 0xa438, 0x0000);
	re_write_phy_ocp(sc, 0xa438, 0x0000);
	re_write_phy_ocp(sc, 0xa438, 0x0000);
	re_write_phy_ocp(sc, 0xa438, 0x0000);
	RE_PHY_SETBIT(sc, 0xad9c, 0x0020);
	re_write_phy_ocp(sc, 0xb87c, 0x8122);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0c00);
	re_write_phy_ocp(sc, 0xb87c, 0x82c8);
	for (i = 0; i < 20; i++)
		re_write_phy_ocp(sc, 0xb87e, mac_cfg3_b87e_value[i]);
	re_write_phy_ocp(sc, 0xb87c, 0x80ef);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0c00);
	for (; i < nitems(mac_cfg3_b87e_value); i++)
		re_write_phy_ocp(sc, 0xb87e, mac_cfg3_b87e_value[i]);
	RE_PHY_SETBIT(sc, 0xa430, 0x0003);
	val = re_read_phy_ocp(sc, 0xb54c) & ~0xffc0;
	re_write_phy_ocp(sc, 0xb54c, val | 0x3700);
	RE_PHY_SETBIT(sc, 0xb648, 0x0040);
	re_write_phy_ocp(sc, 0xb87c, 0x8082);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5d00);
	re_write_phy_ocp(sc, 0xb87c, 0x807c);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5000);
	re_write_phy_ocp(sc, 0xb87c, 0x809d);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x5000);
}

static void
re_phy_config_mac_r25(struct re_softc *sc)
{
	uint16_t val;
	int i;
	static const uint16_t mac_cfg3_a438_value[] =
	    { 0x0043, 0x00a7, 0x00d6, 0x00ec, 0x00f6, 0x00fb, 0x00fd, 0x00ff,
	      0x00bb, 0x0058, 0x0029, 0x0013, 0x0009, 0x0004, 0x0002 };

	static const uint16_t mac_cfg3_b88e_value[] =
	    { 0xc091, 0x6e12, 0xc092, 0x1214, 0xc094, 0x1516, 0xc096, 0x171b,
	      0xc098, 0x1b1c, 0xc09a, 0x1f1f, 0xc09c, 0x2021, 0xc09e, 0x2224,
	      0xc0a0, 0x2424, 0xc0a2, 0x2424, 0xc0a4, 0x2424, 0xc018, 0x0af2,
	      0xc01a, 0x0d4a, 0xc01c, 0x0f26, 0xc01e, 0x118d, 0xc020, 0x14f3,
	      0xc022, 0x175a, 0xc024, 0x19c0, 0xc026, 0x1c26, 0xc089, 0x6050,
	      0xc08a, 0x5f6e, 0xc08c, 0x6e6e, 0xc08e, 0x6e6e, 0xc090, 0x6e12 };

	re_phy_config_mcu(sc, RE_MAC_R25_RCODE_VER);

	RE_PHY_SETBIT(sc, 0xad4e, 0x0010);
	val = re_read_phy_ocp(sc, 0xad16) & ~0x03ff;
	re_write_phy_ocp(sc, 0xad16, val | 0x03ff);
	val = re_read_phy_ocp(sc, 0xad32) & ~0x003f;
	re_write_phy_ocp(sc, 0xad32, val | 0x0006);
	RE_PHY_CLRBIT(sc, 0xac08, 0x1000);
	RE_PHY_CLRBIT(sc, 0xac08, 0x0100);
	val = re_read_phy_ocp(sc, 0xacc0) & ~0x0003;
	re_write_phy_ocp(sc, 0xacc0, val | 0x0002);
	val = re_read_phy_ocp(sc, 0xad40) & ~0x00e0;
	re_write_phy_ocp(sc, 0xad40, val | 0x0040);
	val = re_read_phy_ocp(sc, 0xad40) & ~0x0007;
	re_write_phy_ocp(sc, 0xad40, val | 0x0004);
	RE_PHY_CLRBIT(sc, 0xac14, 0x0080);
	RE_PHY_CLRBIT(sc, 0xac80, 0x0300);
	val = re_read_phy_ocp(sc, 0xac5e) & ~0x0007;
	re_write_phy_ocp(sc, 0xac5e, val | 0x0002);
	re_write_phy_ocp(sc, 0xad4c, 0x00a8);
	re_write_phy_ocp(sc, 0xac5c, 0x01ff);
	val = re_read_phy_ocp(sc, 0xac8a) & ~0x00f0;
	re_write_phy_ocp(sc, 0xac8a, val | 0x0030);
	re_write_phy_ocp(sc, 0xb87c, 0x8157);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0500);
	re_write_phy_ocp(sc, 0xb87c, 0x8159);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0700);
	re_write_phy_ocp(sc, 0xb87c, 0x80a2);
	re_write_phy_ocp(sc, 0xb87e, 0x0153);
	re_write_phy_ocp(sc, 0xb87c, 0x809c);
	re_write_phy_ocp(sc, 0xb87e, 0x0153);

	re_write_phy_ocp(sc, 0xa436, 0x81b3);
	for (i = 0; i < nitems(mac_cfg3_a438_value); i++)
		re_write_phy_ocp(sc, 0xa438, mac_cfg3_a438_value[i]);
	for (i = 0; i < 26; i++)
		re_write_phy_ocp(sc, 0xa438, 0);
	re_write_phy_ocp(sc, 0xa436, 0x8257);
	re_write_phy_ocp(sc, 0xa438, 0x020f);
	re_write_phy_ocp(sc, 0xa436, 0x80ea);
	re_write_phy_ocp(sc, 0xa438, 0x7843);

	re_patch_phy_mcu(sc, 1);
	RE_PHY_CLRBIT(sc, 0xb896, 0x0001);
	RE_PHY_CLRBIT(sc, 0xb892, 0xff00);
	for (i = 0; i < nitems(mac_cfg3_b88e_value); i += 2) {
		re_write_phy_ocp(sc, 0xb88e, mac_cfg3_b88e_value[i]);
		re_write_phy_ocp(sc, 0xb890, mac_cfg3_b88e_value[i + 1]);
	}
	RE_PHY_SETBIT(sc, 0xb896, 0x0001);
	re_patch_phy_mcu(sc, 0);

	RE_PHY_SETBIT(sc, 0xd068, 0x2000);
	re_write_phy_ocp(sc, 0xa436, 0x81a2);
	RE_PHY_SETBIT(sc, 0xa438, 0x0100);
	val = re_read_phy_ocp(sc, 0xb54c) & ~0xff00;
	re_write_phy_ocp(sc, 0xb54c, val | 0xdb00);
	RE_PHY_CLRBIT(sc, 0xa454, 0x0001);
	RE_PHY_SETBIT(sc, 0xa5d4, 0x0020);
	RE_PHY_CLRBIT(sc, 0xad4e, 0x0010);
	RE_PHY_CLRBIT(sc, 0xa86a, 0x0001);
	RE_PHY_SETBIT(sc, 0xa442, 0x0800);
	RE_PHY_SETBIT(sc, 0xa424, 0x0008);
}

static void
re_phy_config_mac_r25b(struct re_softc *sc)
{
	uint16_t val;
	int i;

	re_phy_config_mcu(sc, RE_MAC_R25B_RCODE_VER);

	RE_PHY_SETBIT(sc, 0xa442, 0x0800);
	val = re_read_phy_ocp(sc, 0xac46) & ~0x00f0;
	re_write_phy_ocp(sc, 0xac46, val | 0x0090);
	val = re_read_phy_ocp(sc, 0xad30) & ~0x0003;
	re_write_phy_ocp(sc, 0xad30, val | 0x0001);
	re_write_phy_ocp(sc, 0xb87c, 0x80f5);
	re_write_phy_ocp(sc, 0xb87e, 0x760e);
	re_write_phy_ocp(sc, 0xb87c, 0x8107);
	re_write_phy_ocp(sc, 0xb87e, 0x360e);
	re_write_phy_ocp(sc, 0xb87c, 0x8551);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0800);
	val = re_read_phy_ocp(sc, 0xbf00) & ~0xe000;
	re_write_phy_ocp(sc, 0xbf00, val | 0xa000);
	val = re_read_phy_ocp(sc, 0xbf46) & ~0x0f00;
	re_write_phy_ocp(sc, 0xbf46, val | 0x0300);
	for (i = 0; i < 10; i++) {
		re_write_phy_ocp(sc, 0xa436, 0x8044 + i * 6);
		re_write_phy_ocp(sc, 0xa438, 0x2417);
	}
	RE_PHY_SETBIT(sc, 0xa4ca, 0x0040);
	val = re_read_phy_ocp(sc, 0xbf84) & ~0xe000;
	re_write_phy_ocp(sc, 0xbf84, val | 0xa000);
	re_write_phy_ocp(sc, 0xa436, 0x8170);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x2700;
	re_write_phy_ocp(sc, 0xa438, val | 0xd800);
	RE_PHY_SETBIT(sc, 0xa424, 0x0008);
}

static void
re_phy_config_mac_r25d_1(struct re_softc *sc)
{
	uint16_t val;
	int i;

	re_phy_config_mcu(sc, RE_MAC_R25D_1_RCODE_VER);

	RE_PHY_SETBIT(sc, 0xa442, 0x0800);

	re_patch_phy_mcu(sc, 1);
	RE_PHY_SETBIT(sc, 0xbf96, 0x8000);
	val = re_read_phy_ocp(sc, 0xbf94) & ~0x0007;
	re_write_phy_ocp(sc, 0xbf94, val | 0x0005);
	val = re_read_phy_ocp(sc, 0xbf8e) & ~0x3c00;
	re_write_phy_ocp(sc, 0xbf8e, val | 0x2800);
	val = re_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	re_write_phy_ocp(sc, 0xbcd8, val | 0x4000);
	RE_PHY_SETBIT(sc, 0xbcd8, 0xc000);
	val = re_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	re_write_phy_ocp(sc, 0xbcd8, val | 0x4000);
	val = re_read_phy_ocp(sc, 0xbc80) & ~0x001f;
	re_write_phy_ocp(sc, 0xbc80, val | 0x0004);
	RE_PHY_SETBIT(sc, 0xbc82, 0xe000);
	RE_PHY_SETBIT(sc, 0xbc82, 0x1c00);
	val = re_read_phy_ocp(sc, 0xbc80) & ~0x001f;
	re_write_phy_ocp(sc, 0xbc80, val | 0x0005);
	val = re_read_phy_ocp(sc, 0xbc82) & ~0x00e0;
	re_write_phy_ocp(sc, 0xbc82, val | 0x0040);
	RE_PHY_SETBIT(sc, 0xbc82, 0x001c);
	RE_PHY_CLRBIT(sc, 0xbcd8, 0xc000);
	val = re_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	re_write_phy_ocp(sc, 0xbcd8, val | 0x8000);
	RE_PHY_CLRBIT(sc, 0xbcd8, 0xc000);
	RE_PHY_CLRBIT(sc, 0xbd70, 0x0100);
	RE_PHY_SETBIT(sc, 0xa466, 0x0002);
	re_write_phy_ocp(sc, 0xa436, 0x836a);
	RE_PHY_CLRBIT(sc, 0xa438, 0xff00);
	re_patch_phy_mcu(sc, 0);

	re_write_phy_ocp(sc, 0xb87c, 0x832c);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0500);
	val = re_read_phy_ocp(sc, 0xb106) & ~0x0700;
	re_write_phy_ocp(sc, 0xb106, val | 0x0100);
	val = re_read_phy_ocp(sc, 0xb206) & ~0x0700;
	re_write_phy_ocp(sc, 0xb206, val | 0x0200);
	val = re_read_phy_ocp(sc, 0xb306) & ~0x0700;
	re_write_phy_ocp(sc, 0xb306, val | 0x0300);
	re_write_phy_ocp(sc, 0xb87c, 0x80cb);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0300);
	re_write_phy_ocp(sc, 0xbcf4, 0x0000);
	re_write_phy_ocp(sc, 0xbcf6, 0x0000);
	re_write_phy_ocp(sc, 0xbc12, 0x0000);
	re_write_phy_ocp(sc, 0xb87c, 0x844d);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0200);

	re_write_phy_ocp(sc, 0xb87c, 0x8feb);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	re_write_phy_ocp(sc, 0xb87c, 0x8fe9);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0600);

	val = re_read_phy_ocp(sc, 0xac7e) & ~0x01fc;
	re_write_phy_ocp(sc, 0xac7e, val | 0x00B4);
	re_write_phy_ocp(sc, 0xb87c, 0x8105);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x7a00);
	re_write_phy_ocp(sc, 0xb87c, 0x8117);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3a00);
	re_write_phy_ocp(sc, 0xb87c, 0x8103);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x7400);
	re_write_phy_ocp(sc, 0xb87c, 0x8115);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3400);
	RE_PHY_CLRBIT(sc, 0xad40, 0x0030);
	val = re_read_phy_ocp(sc, 0xad66) & ~0x000f;
	re_write_phy_ocp(sc, 0xad66, val | 0x0007);
	val = re_read_phy_ocp(sc, 0xad68) & ~0xf000;
	re_write_phy_ocp(sc, 0xad68, val | 0x8000);
	val = re_read_phy_ocp(sc, 0xad68) & ~0x0f00;
	re_write_phy_ocp(sc, 0xad68, val | 0x0500);
	val = re_read_phy_ocp(sc, 0xad68) & ~0x000f;
	re_write_phy_ocp(sc, 0xad68, val | 0x0002);
	val = re_read_phy_ocp(sc, 0xad6a) & ~0xf000;
	re_write_phy_ocp(sc, 0xad6a, val | 0x7000);
	re_write_phy_ocp(sc, 0xac50, 0x01e8);
	re_write_phy_ocp(sc, 0xa436, 0x81fa);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x5400);
	val = re_read_phy_ocp(sc, 0xa864) & ~0x00f0;
	re_write_phy_ocp(sc, 0xa864, val | 0x00c0);
	val = re_read_phy_ocp(sc, 0xa42c) & ~0x00ff;
	re_write_phy_ocp(sc, 0xa42c, val | 0x0002);
	re_write_phy_ocp(sc, 0xa436, 0x80e1);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0f00);
	re_write_phy_ocp(sc, 0xa436, 0x80de);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xf000;
	re_write_phy_ocp(sc, 0xa438, val | 0x0700);
	RE_PHY_SETBIT(sc, 0xa846, 0x0080);
	re_write_phy_ocp(sc, 0xa436, 0x80ba);
	re_write_phy_ocp(sc, 0xa438, 0x8a04);
	re_write_phy_ocp(sc, 0xa436, 0x80bd);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xca00);
	re_write_phy_ocp(sc, 0xa436, 0x80b7);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xb300);
	re_write_phy_ocp(sc, 0xa436, 0x80ce);
	re_write_phy_ocp(sc, 0xa438, 0x8a04);
	re_write_phy_ocp(sc, 0xa436, 0x80d1);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xca00);
	re_write_phy_ocp(sc, 0xa436, 0x80cb);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0xbb00);
	re_write_phy_ocp(sc, 0xa436, 0x80a6);
	re_write_phy_ocp(sc, 0xa438, 0x4909);
	re_write_phy_ocp(sc, 0xa436, 0x80a8);
	re_write_phy_ocp(sc, 0xa438, 0x05b8);
	re_write_phy_ocp(sc, 0xa436, 0x8200);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x5800);
	re_write_phy_ocp(sc, 0xa436, 0x8ff1);
	re_write_phy_ocp(sc, 0xa438, 0x7078);
	re_write_phy_ocp(sc, 0xa436, 0x8ff3);
	re_write_phy_ocp(sc, 0xa438, 0x5d78);
	re_write_phy_ocp(sc, 0xa436, 0x8ff5);
	re_write_phy_ocp(sc, 0xa438, 0x7862);
	re_write_phy_ocp(sc, 0xa436, 0x8ff7);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x1400);

	re_write_phy_ocp(sc, 0xa436, 0x814c);
	re_write_phy_ocp(sc, 0xa438, 0x8455);
	re_write_phy_ocp(sc, 0xa436, 0x814e);
	re_write_phy_ocp(sc, 0xa438, 0x84a6);
	re_write_phy_ocp(sc, 0xa436, 0x8163);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0600);
	re_write_phy_ocp(sc, 0xa436, 0x816a);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x0500);
	re_write_phy_ocp(sc, 0xa436, 0x8171);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x1f00);

	val = re_read_phy_ocp(sc, 0xbc3a) & ~0x000f;
	re_write_phy_ocp(sc, 0xbc3a, val | 0x0006);
	for (i = 0; i < 10; i++) {
		re_write_phy_ocp(sc, 0xa436, 0x8064 + i * 3);
		RE_PHY_CLRBIT(sc, 0xa438, 0x0700);
	}
	val = re_read_phy_ocp(sc, 0xbfa0) & ~0xff70;
	re_write_phy_ocp(sc, 0xbfa0, val | 0x5500);
	re_write_phy_ocp(sc, 0xbfa2, 0x9d00);
	re_write_phy_ocp(sc, 0xa436, 0x8165);
	val = re_read_phy_ocp(sc, 0xa438) & ~0x0700;
	re_write_phy_ocp(sc, 0xa438, val | 0x0200);

	re_write_phy_ocp(sc, 0xa436, 0x8019);
	RE_PHY_SETBIT(sc, 0xa438, 0x0100);
	re_write_phy_ocp(sc, 0xa436, 0x8fe3);
	re_write_phy_ocp(sc, 0xa438, 0x0005);
	re_write_phy_ocp(sc, 0xa438, 0x0000);
	re_write_phy_ocp(sc, 0xa438, 0x00ed);
	re_write_phy_ocp(sc, 0xa438, 0x0502);
	re_write_phy_ocp(sc, 0xa438, 0x0b00);
	re_write_phy_ocp(sc, 0xa438, 0xd401);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x2900);

	re_write_phy_ocp(sc, 0xa436, 0x8018);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x1700);

	re_write_phy_ocp(sc, 0xa436, 0x815b);
	val = re_read_phy_ocp(sc, 0xa438) & ~0xff00;
	re_write_phy_ocp(sc, 0xa438, val | 0x1700);

	RE_PHY_CLRBIT(sc, 0xa4e0, 0x8000);
	RE_PHY_CLRBIT(sc, 0xa5d4, 0x0020);
	RE_PHY_CLRBIT(sc, 0xa654, 0x0800);
	RE_PHY_SETBIT(sc, 0xa430, 0x1001);
	RE_PHY_SETBIT(sc, 0xa442, 0x0080);
}

static void
re_phy_config_mac_r25d_2(struct re_softc *sc)
{
	uint16_t val;

	re_phy_config_mcu(sc, RE_MAC_R25D_2_RCODE_VER);

	RE_PHY_SETBIT(sc, 0xa442, 0x0800);

	re_patch_phy_mcu(sc, 1);
	val = re_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	re_write_phy_ocp(sc, 0xbcd8, val | 0x4000);
	RE_PHY_SETBIT(sc, 0xbcd8, 0xc000);
	val = re_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	re_write_phy_ocp(sc, 0xbcd8, val | 0x4000);
	val = re_read_phy_ocp(sc, 0xbc80) & ~0x001f;
	re_write_phy_ocp(sc, 0xbc80, val | 0x0004);
	RE_PHY_SETBIT(sc, 0xbc82, 0xe000);
	RE_PHY_SETBIT(sc, 0xbc82, 0x1c00);
	val = re_read_phy_ocp(sc, 0xbc80) & ~0x001f;
	re_write_phy_ocp(sc, 0xbc80, val | 0x0005);
	val = re_read_phy_ocp(sc, 0xbc82) & ~0x00e0;
	re_write_phy_ocp(sc, 0xbc82, val | 0x0040);
	RE_PHY_SETBIT(sc, 0xbc82, 0x001c);
	RE_PHY_CLRBIT(sc, 0xbcd8, 0xc000);
	val = re_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	re_write_phy_ocp(sc, 0xbcd8, val | 0x8000);
	RE_PHY_CLRBIT(sc, 0xbcd8, 0xc000);
	re_patch_phy_mcu(sc, 0);

	val = re_read_phy_ocp(sc, 0xac7e) & ~0x01fc;
	re_write_phy_ocp(sc, 0xac7e, val | 0x00b4);
	re_write_phy_ocp(sc, 0xb87c, 0x8105);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x7a00);
	re_write_phy_ocp(sc, 0xb87c, 0x8117);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3a00);
	re_write_phy_ocp(sc, 0xb87c, 0x8103);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x7400);
	re_write_phy_ocp(sc, 0xb87c, 0x8115);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x3400);
	re_write_phy_ocp(sc, 0xb87c, 0x8feb);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0500);
	re_write_phy_ocp(sc, 0xb87c, 0x8fea);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0x0700);
	re_write_phy_ocp(sc, 0xb87c, 0x80d6);
	val = re_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	re_write_phy_ocp(sc, 0xb87e, val | 0xef00);
	RE_PHY_CLRBIT(sc, 0xa5d4, 0x0020);
	RE_PHY_CLRBIT(sc, 0xa654, 0x0800);
	RE_PHY_CLRBIT(sc, 0xa448, 0x0400);
	RE_PHY_CLRBIT(sc, 0xa586, 0x0400);
	RE_PHY_SETBIT(sc, 0xa430, 0x1001);
	RE_PHY_SETBIT(sc, 0xa442, 0x0080);
}

static void
re_phy_config_mcu(struct re_softc *sc, uint16_t rcodever)
{
	if (sc->re_rcodever != rcodever) {
		int i;

		re_patch_phy_mcu(sc, 1);

		if (sc->re_type == MAC_R25) {
			re_write_phy_ocp(sc, 0xa436, 0x8024);
			re_write_phy_ocp(sc, 0xa438, 0x8601);
			re_write_phy_ocp(sc, 0xa436, 0xb82e);
			re_write_phy_ocp(sc, 0xa438, 0x0001);

			RE_PHY_SETBIT(sc, 0xb820, 0x0080);

			for (i = 0; i < mac_r25_mcu_len; i++)
				re_write_phy_ocp(sc,
				    mac_r25_mcu[i].reg, mac_r25_mcu[i].val);

			RE_PHY_CLRBIT(sc, 0xb820, 0x0080);

			re_write_phy_ocp(sc, 0xa436, 0);
			re_write_phy_ocp(sc, 0xa438, 0);
			RE_PHY_CLRBIT(sc, 0xb82e, 0x0001);
			re_write_phy_ocp(sc, 0xa436, 0x8024);
			re_write_phy_ocp(sc, 0xa438, 0);
		} else if (sc->re_type == MAC_R25B) {
			for (i = 0; i < mac_r25b_mcu_len; i++)
				re_write_phy_ocp(sc,
				    mac_r25b_mcu[i].reg, mac_r25b_mcu[i].val);
		} else if (sc->re_type == MAC_R25D_1) {
			for (i = 0; i < 2403; i++)
				re_write_phy_ocp(sc,
				    mac_r25d_1_mcu[i].reg,
				    mac_r25d_1_mcu[i].val);
			re_patch_phy_mcu(sc, 0);

			re_patch_phy_mcu(sc, 1);
			for (; i < 2528; i++)
				re_write_phy_ocp(sc,
				    mac_r25d_1_mcu[i].reg,
				    mac_r25d_1_mcu[i].val);
			re_patch_phy_mcu(sc, 0);

			re_patch_phy_mcu(sc, 1);
			for (; i < mac_r25d_1_mcu_len; i++)
				re_write_phy_ocp(sc,
				    mac_r25d_1_mcu[i].reg,
				    mac_r25d_1_mcu[i].val);
		} else if (sc->re_type == MAC_R25D_2) {
			for (i = 0; i < 1269; i++)
				re_write_phy_ocp(sc,
				    mac_r25d_2_mcu[i].reg,
				    mac_r25d_2_mcu[i].val);
			re_patch_phy_mcu(sc, 0);

			re_patch_phy_mcu(sc, 1);
			for (; i < mac_r25d_2_mcu_len; i++)
				re_write_phy_ocp(sc,
				    mac_r25d_2_mcu[i].reg,
				    mac_r25d_2_mcu[i].val);
		} else if (sc->re_type == MAC_R26_1) {
			for (i = 0; i < 6989; i++)
				re_write_phy_ocp(sc,
				    mac_r26_1_mcu[i].reg, mac_r26_1_mcu[i].val);
			re_patch_phy_mcu(sc, 0);

			re_patch_phy_mcu(sc, 1);
			for (; i < mac_r26_1_mcu_len; i++)
				re_write_phy_ocp(sc,
				    mac_r26_1_mcu[i].reg, mac_r26_1_mcu[i].val);
		} else if (sc->re_type == MAC_R26_2) {
			for (i = 0; i < mac_r26_2_mcu_len; i++)
				re_write_phy_ocp(sc,
				    mac_r26_2_mcu[i].reg, mac_r26_2_mcu[i].val);
		} else if (sc->re_type == MAC_R27) {
			for (i = 0; i < 1887; i++)
				re_write_phy_ocp(sc,
				    mac_r27_mcu[i].reg, mac_r27_mcu[i].val);
			re_patch_phy_mcu(sc, 0);

			re_patch_phy_mcu(sc, 1);
			for (; i < mac_r27_mcu_len; i++)
				re_write_phy_ocp(sc,
				    mac_r27_mcu[i].reg, mac_r27_mcu[i].val);
		}

		re_patch_phy_mcu(sc, 0);

		/* Write ram code version. */
		re_write_phy_ocp(sc, 0xa436, 0x801e);
		re_write_phy_ocp(sc, 0xa438, rcodever);
	}
}

void
re_set_macaddr(struct re_softc *sc, const uint8_t *addr)
{
	RE_SETBIT_1(sc, RE_EECMD, RE_EECMD_WRITECFG);
	RE_WRITE_4(sc, RE_MAC0,
	    addr[3] << 24 | addr[2] << 16 | addr[1] << 8 | addr[0]);
	RE_WRITE_4(sc, RE_MAC4,
	    addr[5] <<  8 | addr[4]);
	RE_CLRBIT_1(sc, RE_EECMD, RE_EECMD_WRITECFG);
}

/**
 * @brief Read the mac address from the NIC EEPROM.
 *
 * Note this also calls re_set_macaddr() which programs
 * it into the PPROM; I'm not sure why.
 *
 * Must be called with the driver lock held.
 */
void
re_get_macaddr(struct re_softc *sc, uint8_t *addr)
{
	int i;

	RE_ASSERT_LOCKED(sc);

	for (i = 0; i < ETHER_ADDR_LEN; i++)
		addr[i] = RE_READ_1(sc, RE_MAC0 + i);

	*(uint32_t *)&addr[0] = RE_READ_4(sc, RE_ADDR0);
	*(uint16_t *)&addr[4] = RE_READ_2(sc, RE_ADDR1);

	re_set_macaddr(sc, addr);
}

/**
 * @brief MAC hardware initialisation
 *
 * Must be called with the driver lock held.
 */
static void
re_hw_init(struct re_softc *sc)
{
	uint16_t reg;

	RE_ASSERT_LOCKED(sc);

	re_disable_aspm_clkreq(sc);
	RE_CLRBIT_1(sc, 0xf1, 0x80);

	/* Disable UPS. */
	RE_MAC_CLRBIT(sc, 0xd40a, 0x0010);

	/* Disable MAC MCU. */
	re_disable_aspm_clkreq(sc);
	re_write_mac_ocp(sc, 0xfc48, 0);
	for (reg = 0xfc28; reg < 0xfc48; reg += 2)
		re_write_mac_ocp(sc, reg, 0);
	DELAY(3000);
	re_write_mac_ocp(sc, 0xfc26, 0);

	if (RE_TYPE_R26(sc) || sc->re_type == MAC_R27)
		re_mac_config_ext_mcu(sc, sc->re_type);
	else
		re_mac_config_mcu(sc, sc->re_type);

	/* Disable PHY power saving. */
	if (sc->re_type == MAC_R25)
		re_disable_phy_ocp_pwrsave(sc);

	/* Set PCIe uncorrectable error status. */
	re_write_csi(sc, 0x108,
	    re_read_csi(sc, 0x108) | 0x00100000);
}

void
re_hw_reset(struct re_softc *sc)
{
	/* Disable interrupts using current IMR layout. */
	re_disable_imr_8125(sc);

	/*
	 * Acknowledge/clear pending interrupt status.
	 * For legacy mapping clear all currently pending bits.
	 * For new mapping clear visible V2 bits; layered vectors, if any,
	 * are already masked by re_disable_imr_8125().
	 */
	re_set_isr_8125(sc, re_get_isr_8125(sc));

	/* Clear timer interrupts. */
	RE_WRITE_4(sc, RE_TIMERINT0, 0);
	RE_WRITE_4(sc, RE_TIMERINT1, 0);
	RE_WRITE_4(sc, RE_TIMERINT2, 0);
	RE_WRITE_4(sc, RE_TIMERINT3, 0);

	(void) re_reset(sc);
}

static void
re_disable_phy_ocp_pwrsave(struct re_softc *sc)
{
	if (re_read_phy_ocp(sc, 0xc416) != 0x0500) {
		re_patch_phy_mcu(sc, 1);
		re_write_phy_ocp(sc, 0xc416, 0);
		re_write_phy_ocp(sc, 0xc416, 0x0500);
		re_patch_phy_mcu(sc, 0);
	}
}

static void
re_patch_phy_mcu(struct re_softc *sc, int set)
{
	int i;

	if (set)
		RE_PHY_SETBIT(sc, 0xb820, 0x0010);
	else
		RE_PHY_CLRBIT(sc, 0xb820, 0x0010);

	for (i = 0; i < 1000; i++) {
		if (set) {
			if ((re_read_phy_ocp(sc, 0xb800) & 0x0040) != 0)
				break;
		} else {
			if (!(re_read_phy_ocp(sc, 0xb800) & 0x0040))
				break;
		}
		DELAY(100);
	}
	if (i == 1000)
		dev_err(sc->dev, CE_WARN, "timeout waiting to patch phy mcu");
}

void
re_disable_aspm_clkreq(struct re_softc *sc)
{
	int unlock = 1;

	if ((RE_READ_1(sc, RE_EECMD) & RE_EECMD_WRITECFG) ==
	    RE_EECMD_WRITECFG)
		unlock = 0;

	if (unlock)
		RE_SETBIT_1(sc, RE_EECMD, RE_EECMD_WRITECFG);

	if (RE_TYPE_R26(sc) || sc->re_type == MAC_R27)
		RE_CLRBIT_1(sc, RE_INT_CFG0, 0x08);
	else
		RE_CLRBIT_1(sc, RE_CFG2, RE_CFG2_CLKREQ_EN);
	RE_CLRBIT_1(sc, RE_CFG5, RE_CFG5_PME_STS);

	if (unlock)
		RE_CLRBIT_1(sc, RE_EECMD, RE_EECMD_WRITECFG);
}

static void
re_switch_mcu_ram_page(struct re_softc *sc, int page)
{
	uint16_t val;

	val = re_read_mac_ocp(sc, 0xe446) & ~0x0003;
	val |= page;
	re_write_mac_ocp(sc, 0xe446, val);
}

static int
re_exit_oob(struct re_softc *sc)
{
	int error, i;

	/* Disable RealWoW. */
	re_write_mac_ocp(sc, 0xc0bc, 0x00ff);

	if ((error = re_reset(sc)) != 0)
		return error;

	/* Disable OOB. */
	RE_CLRBIT_1(sc, RE_MCUCMD, RE_MCUCMD_IS_OOB);

	RE_MAC_CLRBIT(sc, 0xe8de, 0x4000);

	for (i = 0; i < 10; i++) {
		DELAY(100);
		if (RE_READ_2(sc, RE_TWICMD) & 0x0200)
			break;
	}

	re_write_mac_ocp(sc, 0xc0aa, 0x07d0);
	re_write_mac_ocp(sc, 0xc0a6, 0x01b5);
	re_write_mac_ocp(sc, 0xc01e, 0x5555);

	for (i = 0; i < 10; i++) {
		DELAY(100);
		if (RE_READ_2(sc, RE_TWICMD) & 0x0200)
			break;
	}

	if (re_read_mac_ocp(sc, 0xd42c) & 0x0100) {
		for (i = 0; i < RE_TIMEOUT; i++) {
			if ((re_read_phy_ocp(sc, 0xa420) & 0x0007) == 2)
				break;
			DELAY(1000);
		}
		RE_MAC_CLRBIT(sc, 0xd42c, 0x0100);
		if (sc->re_type != MAC_R25)
			RE_PHY_CLRBIT(sc, 0xa466, 0x0001);
		RE_PHY_CLRBIT(sc, 0xa468, 0x000a);
	}

	return 0;
}

void
re_write_csi(struct re_softc *sc, uint32_t reg, uint32_t val)
{
	int i;

	RE_WRITE_4(sc, RE_CSIDR, val);
	RE_WRITE_4(sc, RE_CSIAR, (reg & RE_CSIAR_ADDR_MASK) |
	    (RE_CSIAR_BYTE_EN << RE_CSIAR_BYTE_EN_SHIFT) | RE_CSIAR_BUSY);

	for (i = 0; i < 20000; i++) {
		 DELAY(1);
		 if (!(RE_READ_4(sc, RE_CSIAR) & RE_CSIAR_BUSY))
			break;
	}

	DELAY(20);
}

uint32_t
re_read_csi(struct re_softc *sc, uint32_t reg)
{
	int i;

	RE_WRITE_4(sc, RE_CSIAR, (reg & RE_CSIAR_ADDR_MASK) |
	    (RE_CSIAR_BYTE_EN << RE_CSIAR_BYTE_EN_SHIFT));

	for (i = 0; i < 20000; i++) {
		 DELAY(1);
		 if (RE_READ_4(sc, RE_CSIAR) & RE_CSIAR_BUSY)
			break;
	}

	DELAY(20);

	return (RE_READ_4(sc, RE_CSIDR));
}

void
re_write_mac_ocp(struct re_softc *sc, uint16_t reg, uint16_t val)
{
	uint32_t tmp;

	tmp = (reg >> 1) << RE_MACOCP_ADDR_SHIFT;
	tmp += val;
	tmp |= RE_MACOCP_BUSY;
	RE_WRITE_4(sc, RE_MACOCP, tmp);
}

uint16_t
re_read_mac_ocp(struct re_softc *sc, uint16_t reg)
{
	uint32_t val;

	val = (reg >> 1) << RE_MACOCP_ADDR_SHIFT;
	RE_WRITE_4(sc, RE_MACOCP, val);

	return (RE_READ_4(sc, RE_MACOCP) & RE_MACOCP_DATA_MASK);
}

static void
re_write_ephy(struct re_softc *sc, uint16_t reg, uint16_t val)
{
	uint32_t tmp;
	int i;

	tmp = (reg & RE_EPHYAR_ADDR_MASK) << RE_EPHYAR_ADDR_SHIFT;
	tmp |= RE_EPHYAR_BUSY | (val & RE_EPHYAR_DATA_MASK);
	RE_WRITE_4(sc, RE_EPHYAR, tmp);

	for (i = 0; i < 20000; i++) {
		DELAY(1);
		if (!(RE_READ_4(sc, RE_EPHYAR) & RE_EPHYAR_BUSY))
			break;
	}

	DELAY(20);
}

static uint16_t
re_read_ephy(struct re_softc *sc, uint16_t reg)
{
	uint32_t val;
	int i;

	val = (reg & RE_EPHYAR_ADDR_MASK) << RE_EPHYAR_ADDR_SHIFT;
	RE_WRITE_4(sc, RE_EPHYAR, val);

	for (i = 0; i < 20000; i++) {
		DELAY(1);
		val = RE_READ_4(sc, RE_EPHYAR);
		if (val & RE_EPHYAR_BUSY)
			break;
	}

	DELAY(20);

	return (val & RE_EPHYAR_DATA_MASK);
}

static uint16_t
re_check_ephy_ext_add(struct re_softc *sc, uint16_t reg)
{
	uint16_t val;

	val = (reg >> 12);
	re_write_ephy(sc, RE_EPHYAR_EXT_ADDR, val);

	return reg & 0x0fff;
}

static void
re_r27_write_ephy(struct re_softc *sc, uint16_t reg, uint16_t val)
{
	re_write_ephy(sc, re_check_ephy_ext_add(sc, reg), val);
}

void
re_write_phy(struct re_softc *sc, uint16_t addr, uint16_t reg, uint16_t val)
{
	uint16_t off, phyaddr;

	phyaddr = addr ? addr : RE_PHYBASE + (reg / 8);
	phyaddr <<= 4;

	off = addr ? reg : 0x10 + (reg % 8);

	phyaddr += (off - 16) << 1;

	re_write_phy_ocp(sc, phyaddr, val);
}

uint16_t
re_read_phy(struct re_softc *sc, uint16_t addr, uint16_t reg)
{
	uint16_t off, phyaddr;

	phyaddr = addr ? addr : RE_PHYBASE + (reg / 8);
	phyaddr <<= 4;

	off = addr ? reg : 0x10 + (reg % 8);

	phyaddr += (off - 16) << 1;

	return (re_read_phy_ocp(sc, phyaddr));
}

void
re_write_phy_ocp(struct re_softc *sc, uint16_t reg, uint16_t val)
{
	uint32_t tmp;
	int i;

	tmp = (reg >> 1) << RE_PHYOCP_ADDR_SHIFT;
	tmp |= RE_PHYOCP_BUSY | val;
	RE_WRITE_4(sc, RE_PHYOCP, tmp);

	for (i = 0; i < 20000; i++) {
		DELAY(1);
		if (!(RE_READ_4(sc, RE_PHYOCP) & RE_PHYOCP_BUSY))
			break;
	}
}

uint16_t
re_read_phy_ocp(struct re_softc *sc, uint16_t reg)
{
	uint32_t val;
	int i;

	val = (reg >> 1) << RE_PHYOCP_ADDR_SHIFT;
	RE_WRITE_4(sc, RE_PHYOCP, val);

	for (i = 0; i < 20000; i++) {
		DELAY(1);
		val = RE_READ_4(sc, RE_PHYOCP);
		if (val & RE_PHYOCP_BUSY)
			break;
	}

	return (val & RE_PHYOCP_DATA_MASK);
}

int
re_get_link_status(struct re_softc *sc)
{
	return ((RE_READ_2(sc, RE_PHYSTAT) & RE_PHYSTAT_LINK) ? 1 : 0);
}

void
re_wol_config(struct re_softc *sc, int enable)
{
	if (enable)
		RE_MAC_SETBIT(sc, 0xc0b6, 0x0001);
	else
		RE_MAC_CLRBIT(sc, 0xc0b6, 0x0001);

	/* Enable config register write. */
	RE_SETBIT_1(sc, RE_EECMD, RE_EECMD_WRITECFG);

	/* Clear all WOL bits, then set as requested. */
	RE_CLRBIT_1(sc, RE_CFG3, RE_CFG3_WOL_LINK | RE_CFG3_WOL_MAGIC);
	RE_CLRBIT_1(sc, RE_CFG5, RE_CFG5_WOL_LANWAKE |
	    RE_CFG5_WOL_UCAST | RE_CFG5_WOL_MCAST | RE_CFG5_WOL_BCAST);
	if (enable) {
		RE_SETBIT_1(sc, RE_CFG3, RE_CFG3_WOL_MAGIC);
		RE_SETBIT_1(sc, RE_CFG5, RE_CFG5_WOL_LANWAKE);
	}

	/* Config register write done. */
	RE_CLRBIT_1(sc, RE_EECMD, RE_EECMD_WRITECFG);

	if (enable) {
		/* Disable RXDV gate so WOL packets can reach the NIC. */
		RE_CLRBIT_1(sc, RE_PPSW, 0x08);
		DELAY(2000);

		/* Enable power management. */
		RE_SETBIT_1(sc, RE_CFG1, RE_CFG1_PM_EN);
		RE_SETBIT_1(sc, RE_CFG2, RE_CFG2_PMSTS_EN);
	}
}
