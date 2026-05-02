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
#ifndef	__IF_RE_HW_H__
#define	__IF_RE_HW_H__

struct re_softc;
enum re_mac_type;

extern	int re_chipinit(struct re_softc *);
extern	void re_mac_config_mcu(struct re_softc *, enum re_mac_type);
extern	void re_write_mac_ocp(struct re_softc *, uint16_t, uint16_t);
extern	uint16_t re_read_mac_ocp(struct re_softc *, uint16_t);
extern	void re_ephy_config(struct re_softc *);
extern	int re_phy_config(struct re_softc *);
extern	void re_set_macaddr(struct re_softc *, const uint8_t *);
extern	void re_get_macaddr(struct re_softc *, uint8_t *);
extern	void re_hw_reset(struct re_softc *);
extern	void re_config_imtype(struct re_softc *, int);
extern	void re_disable_aspm_clkreq(struct re_softc *);
extern	void re_setup_intr(struct re_softc *, int);
extern	void re_write_csi(struct re_softc *, uint32_t, uint32_t);
extern	uint32_t re_read_csi(struct re_softc *, uint32_t);
extern	void re_write_phy(struct re_softc *, uint16_t, uint16_t, uint16_t);
extern	uint16_t re_read_phy(struct re_softc *, uint16_t, uint16_t);
extern	void re_write_phy_ocp(struct re_softc *, uint16_t, uint16_t);
extern	uint16_t re_read_phy_ocp(struct re_softc *sc, uint16_t reg);
extern	int re_get_link_status(struct re_softc *);
extern	void re_wol_config(struct re_softc *, int);

#endif	/* __IF_RE_HW_H__ */
