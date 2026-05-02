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
#ifndef	__IF_RE_CHIP_H__
#define	__IF_RE_CHIP_H__


struct re_softc;
typedef struct re_tx_ring re_tx_ring_t;


void re_init_locked(struct re_softc *sc);
void re_check_link_status(void *arg);
void re_stop_locked(struct re_softc *sc);
uint32_t re_get_isr_8125(struct re_softc *sc);
void re_set_isr_8125(struct re_softc *sc, uint32_t val);
void re_enable_imr_8125(struct re_softc *sc);
void re_disable_imr_8125(struct re_softc *sc);
void re_clear_hw_isr_v2(struct re_softc *sc, uint32_t intr_idx);

void re_enable_hw_layered_interrupt(struct re_softc *sc, uint32_t intr_idx);
void re_disable_hw_layered_interrupt(struct re_softc *sc, uint32_t intr_idx);

void re_setup_interrupt_mask(struct re_softc *sc);

void re_set_rx_packet_filter(struct re_softc *sc);
int re_apply_adv_caps(struct re_softc *sc);

int re_dump_tally_counter(struct re_softc *sc);

uint32_t re_mcast_hash_index(const uint8_t *mca);
boolean_t re_adv_caps_valid(uint32_t adv_caps);

int re_read_thermal_sensor(struct re_softc *sc);
void re_disable_extend_tally_couter(struct re_softc *sc);
int re_check_mac_version(struct re_softc *sc);
void re_init_software_variable(struct re_softc *sc);
void re_setup_rings_regs(struct re_softc *sc);
void re_doorbell(struct re_softc *sc, re_tx_ring_t *ring);

#endif	/* __IF_RE_CHIP_H__ */
