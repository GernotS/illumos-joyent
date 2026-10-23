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



#ifndef _RE_H
#define	_RE_H


#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/stream.h>
#include <sys/strsun.h>
#include <sys/strsubr.h>
#include <sys/stat.h>
#include <sys/pci.h>
#include <sys/pci_cap.h>
#include <sys/pcie.h>
#include <sys/note.h>
#include <sys/modctl.h>
#include <sys/kstat.h>
#include <sys/ethernet.h>
#include <sys/vlan.h>
#include <sys/errno.h>
#include <sys/dlpi.h>
#include <sys/devops.h>
#include <sys/debug.h>
#include <sys/conf.h>
#include <sys/sensors.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <inet/common.h>
#include <inet/ip.h>
#include <sys/param.h>
#include <sys/pattr.h>
#include <sys/cpuvar.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/firmload.h>
#include <sys/mac_provider.h>
#include <sys/mac_ether.h>
#include <sys/mac_client.h>
#include <sys/cmn_err.h>
#include <sys/random.h>
#include <sys/sysmacros.h>
#include <sys/atomic.h>
#include <sys/time.h>
#include <sys/miiregs.h>

#include <core/if_rereg.h>
#include <core/if_revar.h>
#include <core/if_re_hw.h>
#include <core/if_re_vendor.h>
#include <core/re_chip.h>


#define RE_MOD_NAME "re"
#define RE_RX_POLL_INTR -1

#define	RE_CYCLIC_PERIOD	(1000000000)	/* ~1s */

#define RE_RSS_KEY_SIZE 40 /* size of RSS Hash Key in bytes */
#define RE_MAX_INDIRECTION_TABLE_ENTRIES 128
#define RE_NTXSEGS		35

#define RE_RESET_TX_TIMEOUT	0x00000001
#define RE_RESET_INTR_ERR	0x00000002

enum re_attach_state {
	RE_ATTACH_PCI_CONFIG	= 1U << 0,	/* PCI config setup */
	RE_ATTACH_REGS_MAP	= 1U << 1,	/* Registers mapped */
	RE_ATTACH_ALLOC_INTR	= 1U << 2,	/* Interrupts allocated */
	RE_ATTACH_ALLOC_RINGS	= 1U << 3,	/* Rings allocated */
	RE_ATTACH_ADD_INTR	= 1U << 4,	/* Intr handlers added */
	RE_ATTACH_LOCKS		= 1U << 5,	/* Locks initialized */
	RE_ATTACH_INIT		= 1U << 6,	/* Device initialized ????*/
	RE_ATTACH_STATS		= 1U << 7,	/* Kstats created */
	RE_ATTACH_MAC			= 1U << 8,	/* MAC registered */
	RE_ATTACH_ENABLE_INTR	= 1U << 9,	/* DDI interrupts enabled */
	RE_ATTACH_RX_DATA_INIT	= 1U << 10,
	RE_ATTACH_TX_DATA_INIT	= 1U << 11,
	RE_ATTACH_MAC_START	= 1U << 12,
	RE_ATTACH_IRM = 1U << 13,
	RE_ATTACH_KSTATS = 1U << 14
};

enum re_speed_flags {
	RE_SPEED_2P5G = 1U << 0,
	RE_SPEED_5G = 1U << 1,
	RE_SPEED_10G = 1U << 2
};

enum re_link_caps {
	RE_CAP_10HDX     = 1U << 0,
	RE_CAP_10FDX     = 1U << 1,
	RE_CAP_100HDX    = 1U << 2,
	RE_CAP_100FDX    = 1U << 3,
	RE_CAP_1000HDX   = 1U << 4,
	RE_CAP_1000FDX   = 1U << 5,
	RE_CAP_2500FDX   = 1U << 6,
	RE_CAP_5000FDX   = 1U << 7,
	RE_CAP_10GFDX    = 1U << 8
};

struct TxDesc {
	uint32_t opts1;
	uint32_t opts2;
	uint64_t addr;
	uint32_t reserved0;
	uint32_t reserved1;
	uint32_t reserved2;
	uint32_t reserved3;
};

struct RxDesc {
	uint32_t opts1;
	uint32_t opts2;
	uint64_t addr;
};

struct RxDescV3 {
	union {
		struct {
			uint32_t rsv1;
			uint32_t rsv2;
		} RxDescDDWord1;
	};

	union {
		struct {
			uint32_t RSSResult;
			uint16_t HeaderBufferLen;
			uint16_t HeaderInfo;
		} RxDescNormalDDWord2;

		struct {
			uint32_t rsv5;
			uint32_t rsv6;
		} RxDescDDWord2;
	};

	union {
		uint64_t addr;

		struct {
			uint32_t TimeStampLow;
			uint32_t TimeStampHigh;
		} RxDescTimeStamp;

		struct {
			uint32_t rsv8;
			uint32_t rsv9;
		} RxDescDDWord3;
	};

	union {
		struct {
			uint32_t opts2;
			uint32_t opts1;
		} RxDescNormalDDWord4;

		struct {
			uint16_t TimeStampHHigh;
			uint16_t rsv11;
			uint32_t opts1;
		} RxDescPTPDDWord4;
	};
};

struct RxDescV4 {
	union {
		uint64_t addr;

		struct {
			uint32_t RSSInfo;
			uint32_t RSSResult;
		} RxDescNormalDDWord1;
	};

	struct {
		uint32_t opts2;
		uint32_t opts1;
	} RxDescNormalDDWord2;
};

typedef enum re_intr_mode {
	RE_INTR_MODE_HW = 0,
	RE_INTR_MODE_TIMER
} re_intr_mode_t;

typedef enum re_intr_vec_type {
	RE_INTR_VEC_RX = 0,
	RE_INTR_VEC_TX,
	RE_INTR_VEC_LINK,
	RE_INTR_VEC_RXTX
} re_intr_vec_type_t;

typedef struct re_rx_ring_kstats {
	kstat_named_t ipackets64;
	kstat_named_t rbytes64;
	kstat_named_t rx_desc_err;
	kstat_named_t rx_allocb_fail;
	kstat_named_t rx_bind_fail;
	kstat_named_t rx_copy_pkts;
	kstat_named_t rx_bind_pkts;
	kstat_named_t rx_recycled;

	kstat_named_t rx_no_sof_no_eof;
	kstat_named_t rx_restart_assembly;
	kstat_named_t rx_fragment_pkts;
	kstat_named_t rx_single_desc_pkts;
} re_rx_ring_kstats_t;

typedef struct re_tx_ring_kstats {
	kstat_named_t opackets64;
	kstat_named_t obytes64;
	kstat_named_t tx_reclaim;
	kstat_named_t tx_encap_fail;
	kstat_named_t tx_bind_fail;
	kstat_named_t tx_copy_pkts;
	kstat_named_t tx_bind_pkts;
	kstat_named_t tx_ring_full;

	kstat_named_t tx_hw_csum_pkts;
	kstat_named_t tx_lso_pkts;
	kstat_named_t tx_lso_bytes;
	kstat_named_t tx_lso_patch_pkts;
	kstat_named_t tx_ipv4_csum_pkts;
	kstat_named_t tx_tcp_csum_pkts;
	kstat_named_t tx_udp_csum_pkts;
	kstat_named_t tx_prepare_fail;
} re_tx_ring_kstats_t;

typedef struct re_dma_buffer {
	caddr_t re_db_va;
	ddi_acc_handle_t re_acc_hdl;
	ddi_dma_handle_t re_dma_hdl;
	ddi_dma_cookie_t cookie;
	uint32_t ncookies;
	size_t re_db_alloc_len;
	boolean_t re_bound;
} re_dma_buffer_t;

typedef struct re_rx_buffer {
	struct re_rx_data *rx_data;
	mblk_t *re_rx_mp;
	re_dma_buffer_t rx_dma_buf;
	frtn_t rrb_free_rtn;
	boolean_t rrb_loaned;
} re_rx_buffer_t;

/*
 * RX buffers of one ring for one start/stop cycle.
 *
 * Buffers can be loaned up the stack and may come back long after the ring
 * was stopped.  The buffers therefore live in their own allocation, which
 * the ring drops ("retires") when it stops.  If buffers are still loaned at
 * that point, the last one returned through re_rx_recycle() frees the
 * whole re_rx_data_t.  sc->rx_pending counts retired but not yet freed
 * instances; detach is refused while it is non-zero.
 */
typedef struct re_rx_data {
	struct re_softc *rxd_sc;
	struct re_rx_ring *rxd_ring;	/* NULL once retired */

	kmutex_t rxd_lock;
	kcondvar_t rxd_cv;

	re_rx_buffer_t *rxd_bufs;
	re_rx_buffer_t **rxd_free_list;
	re_rx_buffer_t **rxd_desc_bufs;
	uint32_t rxd_num_bufs;
	uint32_t rxd_num_desc;
	uint32_t rxd_nfree;		/* protected by rxd_lock */
	uint32_t rxd_loaned;		/* protected by rxd_lock */
	boolean_t rxd_retired;		/* protected by rxd_lock */
} re_rx_data_t;

typedef struct re_rx_ring {
	struct re_softc *sc;
	mac_ring_handle_t rh;

	re_rx_data_t *rx_data;		/* protected by rx_lock */
	kmutex_t rx_lock;
	ddi_taskq_t *rx_taskq;
	boolean_t rx_taskq_scheduled;
	uint64_t ring_gen;
	uint32_t num_desc;
	uint32_t num_bufs;
	uint32_t rx_cons;

	/* in-progress fragmented RX packet assembly */
	mblk_t		*rrx_assem_head;
	mblk_t		*rrx_assem_tail;
	uint32_t	rrx_assem_bytes;

	boolean_t	rrx_ip_csum_valid;
	boolean_t	rrx_ip_csum_ok;
	boolean_t	rrx_l4_csum_valid;
	boolean_t	rrx_l4_csum_ok;

	uint32_t desc_own_mask;
	uint32_t desc_eor_mask;
	uint32_t desc_sof_mask;
	uint32_t desc_eof_mask;

	boolean_t rx_lock_init;
	boolean_t rx_kstat_init;

	kstat_t *rx_ksp;
	re_rx_ring_kstats_t rx_kstat;

	uint32_t index;
	uint32_t intr_idx;
	ddi_intr_handle_t intr_handle;

	re_dma_buffer_t rx_dma_desc;
	void *rx_desc;
	size_t rx_desc_stride;

	uint16_t rdsar_reg;
} re_rx_ring_t;

typedef struct re_tx_buffer {
	struct re_tx_ring	*tx_ring;
	list_node_t		tx_node;

	mblk_t			*re_tx_mp;

	ddi_dma_handle_t	 tx_bind_hdl;
	ddi_dma_cookie_t	 tx_cookies[RE_NTXSEGS];
	uint32_t		 tx_ncookies;
	uint32_t tx_last_idx;
	re_dma_buffer_t		 tx_copy_buf;

	uint32_t		 tx_ndescs;
	boolean_t		 tx_bound;
	boolean_t		 tx_copied;
} re_tx_buffer_t;

typedef struct re_tx_ring {
	struct re_softc *sc;
	mac_ring_handle_t th;

	re_dma_buffer_t tx_dma_desc;
	struct TxDesc *tx_desc;

	ddi_taskq_t *tx_taskq;
	boolean_t tx_taskq_scheduled;

	kmutex_t tx_lock;

	kstat_t *tx_ksp;
	re_tx_ring_kstats_t tx_kstat;

	re_tx_buffer_t *tx_arena;
	re_tx_buffer_t **tx_work_list;
	list_t tx_free_list;
	boolean_t tx_free_list_init;
	boolean_t tx_lock_init;
	boolean_t tx_kstat_init;

	uint32_t num_desc;
	uint32_t num_bufs;

	uint32_t cur_tx;
	uint32_t dirty_tx;

	/* desc mask */
	uint32_t desc_own_mask;
	uint32_t desc_eor_mask;
	uint32_t desc_sof_mask;
	uint32_t desc_eof_mask;


	hrtime_t	tx_last_reclaim_time;
	hrtime_t	tx_last_submit_time;
	uint64_t	tx_watchdog_timeouts;

	uint32_t index;
	uint32_t intr_idx;
	ddi_intr_handle_t	 intr_handle;

	uint16_t		 tdsar_reg;
} re_tx_ring_t;

typedef struct re_intr_vector {
	struct re_softc		*sc;
	re_rx_ring_t		*rx_ring;
	re_tx_ring_t		*tx_ring;

	re_intr_vec_type_t	type;

	uint32_t		status_mask;
	ddi_taskq_t		*taskq;

	kmutex_t		lock;
	boolean_t		scheduled;
	uint32_t pending_status;
	uint_t			intr_idx;
} re_intr_vector_t;

typedef struct re_tally_counter {
	re_dma_buffer_t tally_buf;
	struct re_stats *stats;
} re_tally_counter_t;

typedef struct re_pci_cfg {
	uint16_t command;		/* saved during attach	*/
	uint16_t vendor;		/* vendor-id		*/
	uint16_t device;		/* device-id		*/
	uint16_t subven;		/* subsystem-vendor-id	*/
	uint16_t subdev;		/* subsystem-id		*/
	uint8_t revision;		/* revision-id		*/
	uint8_t clsize;		/* cache-line-size	*/
	uint8_t ilr;			/* interrupt line */
} re_pci_cfg_t;

typedef struct re_sensor {
	boolean_t isn_valid;
	id_t isn_reg_ksensor;
} re_sensors_t;

typedef struct re_softc_kstats {
	kstat_named_t re_type;
	kstat_named_t re_device_id;
	kstat_named_t mtu;
	kstat_named_t link_state;
	kstat_named_t link_speed;
	kstat_named_t link_duplex;
	kstat_named_t num_rx_rings;
	kstat_named_t num_tx_rings;
	kstat_named_t re_intr_type;
	kstat_named_t intr_cnt;
	kstat_named_t hw_isr_ver;
	kstat_named_t run_flags;
	kstat_named_t suspended;

	kstat_named_t rx_pkts;
	kstat_named_t tx_pkts;
	kstat_named_t rx_octets;
	kstat_named_t tx_octets;
	kstat_named_t rx_multicast64;
	kstat_named_t tx_multicast64;
	kstat_named_t rx_bcasts;
	kstat_named_t tx_broadcast64;
	kstat_named_t rx_errs;
	kstat_named_t tx_errs;
	kstat_named_t missed_pkts;
	kstat_named_t tx_all_collision;
	kstat_named_t rdu;
	kstat_named_t tx_underrun32;
	kstat_named_t rx_framealign_errs;
	kstat_named_t tx_onecoll;
	kstat_named_t tx_multicolls;
	kstat_named_t tx_deferred;
	kstat_named_t tx_late_collision;
	kstat_named_t rx_frame_too_long;
	kstat_named_t rx_runt;
	kstat_named_t re_reset_counter;
} re_softc_kstats_t;

struct re_softc {
	dev_info_t *dev;
	mac_handle_t mh;
	mac_group_handle_t re_rxg_hdl;
	/* pci */
	ddi_acc_handle_t cfg_handle;
	re_pci_cfg_t pci_cfg;

	/* mmio */
	ddi_acc_handle_t io_handle;
	caddr_t io_regs;

	/* IRM handler */
	ddi_cb_handle_t		cb_hdl;

	/* chip stats */
	re_tally_counter_t	re_tally;

	/* periodic tasks */
	ddi_periodic_t esd_check;
	ddi_periodic_t link_status;

	/* temp sensor */
	re_sensors_t sensor;

	uint32_t force_msi;

	/* interrupts */
	ddi_intr_handle_t 	*htable;
	uint32_t re_intr_type;
	int re_intr_cap;
	uint_t intr_pri;
	uint16_t intr_cnt;
	uint16_t intr_cnt_max;
	uint16_t msix_min_reqired;
	uint16_t intr_allocated;
	uint16_t intr_used;
	uint16_t intr_avail; /* tracking of IRM availability */
	uint8_t irm_registered;
	uint8_t irm_pending;
	uint32_t intr_mask;
	uint32_t intr_mask_rx;
	uint32_t intr_mask_tx;
	uint32_t intr_mask_link;
	uint32_t intr_mask_err;
	re_intr_vector_t intr_vec0;
	re_intr_vector_t *intr_vecs;
	uint_t intr_vec_cnt;
	uint32_t rx_intr_bytes;
	boolean_t intr_timer_mode;
	uint32_t timer_count;
	uint32_t timer_count_v2;
	uint32_t timer_intr_mask;
	uint32_t timer_int_enable;

	/* global mutex */
	kmutex_t mtx;

	/* stats */
	kstat_t *sc_ksp;
	re_softc_kstats_t sc_kstat;

	/* reset helper */
	ddi_taskq_t	*reset_tq;
	ddi_periodic_t	watchdog_periodic;
	uint64_t re_reset_counter;
	boolean_t	reset_pending;
	boolean_t	reset_running;
	uint32_t	reset_reason;

	/* used for multicast/promisc mode set */
	boolean_t re_promisc;
	boolean_t re_allmulti;
	uint32_t  re_mc_count;
	uint8_t mcast_refs[64];
	uint8_t mcast_hash[8];

	/* link params */
	uint32_t mtu;
	link_state_t re_link_state;
	uint16_t supported_speeds;
	link_duplex_t re_link_duplex;
	uint16_t re_link_speed;
	uint8_t suspended;
	uint32_t attach_state;
	uint8_t org_mac_addr[ETHERADDRL];
	boolean_t autoneg;
	link_flowctrl_t re_flowctrl;
	uint32_t media_status;
	uint32_t hw_caps;
	uint32_t adv_caps;

	/* rings */
	uint8_t re_rx_ring_desc_type;
	uint16_t HwSuppNumTxQueues;
	uint16_t HwSuppNumRxQueues;
	uint16_t num_tx_rings;
	uint16_t num_rx_rings;
	re_rx_ring_t *rx_rings;
	re_tx_ring_t *tx_rings;
	volatile uint32_t rx_pending;	/* retired re_rx_data_t not yet freed */

	/* chip specific variables */
	uint16_t re_rcodever;
	uint64_t re_mcodever;
	boolean_t sc_stopped;
	uint8_t re_unit;
	uint8_t re_type;
	int re_device_id;
	uint8_t HwSuppIsrVer;
	uint8_t HwCurrIsrVer;
	uint8_t use_new_intr_mapping;
	uint8_t HwSuppIntMitiVer;

	/* rss */
	uint8_t Enable_Rss;
	uint32_t rss_ctrl;
	uint8_t rss_key[RE_RSS_KEY_SIZE];
	uint8_t rss_i_table[RE_MAX_INDIRECTION_TABLE_ENTRIES];
	uint16_t rss_queue_num_sel_r;
};

#define SPEED_10	10
#define SPEED_100	100
#define SPEED_1000	1000
#define SPEED_2500	2500
#define SPEED_5000  5000
#define SPEED_10000 10000

enum rx_ring_desc_type {
	RE_RX_RING_DESC_TYPE_1,
	RE_RX_RING_DESC_TYPE_3,
	RE_RX_RING_DESC_TYPE_4
};

#ifdef	DEBUG
#define	RE_DMA_SYNC(buf, flag)		ASSERT0(ddi_dma_sync((buf)->re_dma_hdl, \
					    0, 0, flag))
#else
#define	RE_DMA_SYNC(buf, flag)		(void) ddi_dma_sync((buf)->re_dma_hdl, \
					    0, 0, flag)
#endif	/* DEBUG */

#define RE_LOCK(_sc)		mutex_enter(&(_sc)->mtx)
#define RE_UNLOCK(_sc)		mutex_exit(&(_sc)->mtx)
#define RE_LOCK_INIT(_sc)	mutex_init(&(_sc)->mtx, NULL, MUTEX_DRIVER, \
				    DDI_INTR_PRI((_sc)->intr_pri))
#define RE_LOCK_DESTROY(_sc)	mutex_destroy(&(_sc)->mtx)
#define RE_LOCK_ASSERT(_sc)	ASSERT(MUTEX_HELD(&(_sc)->mtx))
#define RE_ASSERT_LOCKED(sc)    ASSERT(MUTEX_HELD(&(sc)->mtx))
#define RE_ASSERT_UNLOCKED(sc)  ASSERT(!MUTEX_HELD(&(sc)->mtx))

/*
 * register space access macros
 */

#define	REG32(sc, reg)	((uint32_t *)(sc->io_regs+(reg)))
#define	REG16(sc, reg)	((uint16_t *)(sc->io_regs+(reg)))
#define	REG8(sc, reg)	((uint8_t *)(sc->io_regs+(reg)))

#define RE_WRITE_4(sc, reg, val)	ddi_put32(sc->io_handle, REG32(sc, reg), val)
#define RE_WRITE_2(sc, reg, val)	ddi_put16(sc->io_handle, REG16(sc, reg), val)
#define RE_WRITE_1(sc, reg, val)	ddi_put8(sc->io_handle, REG8(sc, reg), val)

#define RE_READ_4(sc, reg)	ddi_get32(sc->io_handle, REG32(sc, reg))
#define RE_READ_2(sc, reg)	ddi_get16(sc->io_handle, REG16(sc, reg))
#define RE_READ_1(sc, reg)	ddi_get8(sc->io_handle, REG8(sc, reg))

/* rss */
void re_init_rss(struct re_softc *sc);
void re_config_rss(struct re_softc *sc);
void re_disable_rss(struct re_softc *sc);
uint32_t re_rss_indir_tbl_entries(struct re_softc *sc);
void re_choose_intr_layout(struct re_softc *sc);

/* temp sensor */
void re_sensor_init(struct re_softc *);
void re_sensor_fini(struct re_softc *);

/* interupts */
int re_irm_register(struct re_softc *sc);
void re_irm_unregister(struct re_softc *sc);
int re_intr_enable_all(struct re_softc *sc);
void re_intr_disable_all(struct re_softc *sc);
int re_msix_id_to_tx_ring_id(struct re_softc *sc, int intr_id);
int re_alloc_intr(struct re_softc *sc);
void re_free_intr(struct re_softc *sc);
int re_intr_setup_handlers(struct re_softc *sc);
void re_intr_teardown_handlers(struct re_softc *sc);

/* kstat */
boolean_t re_softc_kstat_init(struct re_softc *sc);
void re_softc_kstat_fini(struct re_softc *sc);
boolean_t re_rx_ring_kstats_init(struct re_softc *sc, re_rx_ring_t *rr);
void re_rx_ring_stats_fini(struct re_softc *sc, re_rx_ring_t *rr);
boolean_t re_tx_ring_kstat_init(struct re_softc *sc, re_tx_ring_t *tr);
void re_tx_ring_kstat_fini(struct re_softc *sc, re_tx_ring_t *tr);

/* rings */
int re_stop_datapath(struct re_softc *sc, clock_t ticks);
int re_alloc_rings(struct re_softc *sc);
void re_free_rings(struct re_softc *sc);
mblk_t * re_ring_tx(void *arg, mblk_t *mp);
mblk_t * re_ring_rx(re_rx_ring_t *rr, int poll_bytes);
int re_alloc_rings_data(struct re_softc *sc);
void re_free_rings_data(struct re_softc *sc);
void re_map_rings_to_dev(struct re_softc *sc);
void re_start_datapath(struct re_softc *sc);
void re_txeof(re_tx_ring_t *tr);
uint32_t re_tx_desc_free(re_tx_ring_t *tr);
boolean_t re_txeof_should_wake(re_tx_ring_t *tr);
void re_tx_desc_init_ring(re_tx_ring_t *tr);
int re_rx_ring_reinit(struct re_softc *sc, re_rx_ring_t *rr);

void re_schedule_reset_locked(struct re_softc *sc, uint32_t reason);
/* gld */
boolean_t re_mac_register(struct re_softc *sc);

#define RE_IS_L3_IP(_meoi) \
	((_meoi).meoi_l3proto == ETHERTYPE_IP)

#define RE_IS_L3_IPV6(_meoi) \
	((_meoi).meoi_l3proto == ETHERTYPE_IPV6)

#define RE_IS_L4_TCP(_meoi) \
	((_meoi).meoi_l4proto == IPPROTO_TCP)

#define RE_IS_L4_UDP(_meoi) \
	((_meoi).meoi_l4proto == IPPROTO_UDP)


#ifdef __cplusplus
}
#endif

#endif	/* _RE_H */
