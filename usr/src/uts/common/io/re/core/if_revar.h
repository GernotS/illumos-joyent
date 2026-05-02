/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019, 2020, 2025 Kevin Lo <kevlo@openbsd.org>
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

#ifndef	__IF_REVAR_H__
#define	__IF_REVAR_H__

enum re_mac_type {
	MAC_UNKNOWN = 1,
	MAC_R25,
	MAC_R25B,
	MAC_R25D_1,
	MAC_R25D_2,
	MAC_R26_1,
	MAC_R26_2,
	MAC_R27
};

/*
 * Register space access macros.
 */

#define RE_SETBIT_4(sc, reg, val)	\
	RE_WRITE_4(sc, reg, RE_READ_4(sc, reg) | (val))
#define RE_SETBIT_2(sc, reg, val)	\
	RE_WRITE_2(sc, reg, RE_READ_2(sc, reg) | (val))
#define RE_SETBIT_1(sc, reg, val)	\
	RE_WRITE_1(sc, reg, RE_READ_1(sc, reg) | (val))

#define RE_CLRBIT_4(sc, reg, val)	\
	RE_WRITE_4(sc, reg, RE_READ_4(sc, reg) & ~(val))
#define RE_CLRBIT_2(sc, reg, val)	\
	RE_WRITE_2(sc, reg, RE_READ_2(sc, reg) & ~(val))
#define RE_CLRBIT_1(sc, reg, val)	\
	RE_WRITE_1(sc, reg, RE_READ_1(sc, reg) & ~(val))

#define RE_EPHY_SETBIT(sc, reg, val)	\
	re_write_ephy(sc, reg, re_read_ephy(sc, reg) | (val))

#define RE_EPHY_CLRBIT(sc, reg, val)	\
	re_write_ephy(sc, reg, re_read_ephy(sc, reg) & ~(val))

#define RE_PHY_SETBIT(sc, reg, val)	\
	re_write_phy_ocp(sc, reg, re_read_phy_ocp(sc, reg) | (val))

#define RE_PHY_CLRBIT(sc, reg, val)	\
	re_write_phy_ocp(sc, reg, re_read_phy_ocp(sc, reg) & ~(val))

#define RE_MAC_SETBIT(sc, reg, val)	\
	re_write_mac_ocp(sc, reg, re_read_mac_ocp(sc, reg) | (val))

#define RE_MAC_CLRBIT(sc, reg, val)	\
	re_write_mac_ocp(sc, reg, re_read_mac_ocp(sc, reg) & ~(val))

#endif	/* __IF_REVAR_H__ */
