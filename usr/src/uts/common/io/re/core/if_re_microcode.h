/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019, 2020, 2025 Kevin Lo <kevlo@openbsd.org>
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

/*	$OpenBSD: if_rgereg.h,v 1.15 2025/09/19 00:41:14 kevlo Exp $	*/

#ifndef	__IF_RE_MICROCODE_H__
#define	__IF_RE_MICROCODE_H__

/*
 * The original huge arrays of code were distributed among the corresponding files
 * if_re_microcode_8125.c if_re_microcode_8126.c if_re_microcode_8127.c
 *
 * In if_re_hw nitems replaced with len variable
 * Example:
 * nitems(rtl8125_mac_bps) in if_re_hw.c was replaced with rtl8125_mac_bps_len
 *
 */

struct re_hw_regaddr_array {
	uint16_t reg;
	uint16_t val;
};

extern const struct re_hw_regaddr_array rtl8125_mac_bps[];
extern const size_t rtl8125_mac_bps_len;

extern const struct re_hw_regaddr_array rtl8125b_mac_bps[];
extern const size_t rtl8125b_mac_bps_len;

extern const struct re_hw_regaddr_array rtl8125d_2_mac_bps[];
extern const size_t rtl8125d_2_mac_bps_len;

struct re_mac_bps {
	int		count;
	const uint16_t	*regs;
	const uint16_t	*vals;
};

extern const uint16_t rtl8126_1_mac_bps_regs[];

extern const uint16_t rtl8126_1_mac_bps_vals[];

extern const struct re_mac_bps rtl8126_1_mac_bps;

extern const uint16_t rtl8126_2_mac_bps_regs[];

extern const uint16_t rtl8126_2_mac_bps_vals[];

extern const struct re_mac_bps rtl8126_2_mac_bps;

extern const uint16_t rtl8127_mac_bps_regs[] ;

extern const uint16_t rtl8127_mac_bps_vals[];

extern const struct re_mac_bps rtl8127_mac_bps;

extern const struct re_hw_regaddr_array mac_r25_ephy[];
extern const size_t mac_r25_ephy_len;

extern const struct re_hw_regaddr_array mac_r25b_ephy[];
extern const size_t mac_r25b_ephy_len;

extern const struct re_hw_regaddr_array mac_r27_ephy[];
extern const size_t mac_r27_ephy_len;

extern const struct re_hw_regaddr_array mac_r25d_2_mcu[];
extern const size_t mac_r25d_2_mcu_len;

extern const struct re_hw_regaddr_array mac_r26_1_mcu[];
extern const size_t mac_r26_1_mcu_len;

extern const struct re_hw_regaddr_array mac_r26_2_mcu[];
extern const size_t mac_r26_2_mcu_len;

extern const struct re_hw_regaddr_array mac_r27_mcu[];
extern const size_t mac_r27_mcu_len;

/*
 * These should just be arrays as well, but to make diffing with
 * OpenBSD easier I'm going to leave them as-is until kevlo@ wants
 * to sync them!
 */

struct re_mcu_reg {
	uint16_t reg;
	uint16_t val;
};

extern const struct re_mcu_reg mac_r25_mcu[];
extern const size_t mac_r25_mcu_len;
extern const struct re_mcu_reg mac_r25b_mcu[];
extern const size_t mac_r25b_mcu_len;
extern const struct re_mcu_reg mac_r25d_1_mcu[];
extern const size_t mac_r25d_1_mcu_len;

#endif	/* __IF_RE_MICROCODE_H__ */
