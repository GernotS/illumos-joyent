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

#ifndef	__IF_REREG_H__
#define	__IF_REREG_H__

/* For now, a single MSI message, no multi-RX/TX ring support */
#define	RE_MSI_MESSAGES	1

#define RE_MAC0		0x0000
#define RE_MAC4		0x0004
#define RE_MAR0		0x0008
#define RE_MAR4		0x000c
#define	RE_DTCCR_LO		0x0010
#define	RE_DTCCR_CMD		(1U << 3)
#define	RE_DTCCR_HI		0x0014
#define RE_TXDESC_ADDR_LO	0x0020
#define RE_TXDESC_ADDR_HI	0x0024
#define RE_INT_CFG0		0x0034
#define RE_CMD			0x0037
#define RE_IMR			0x0038
#define RE_ISR			0x003c
#define RE_TXCFG		0x0040
#define RE_RXCFG		0x0044
#define RE_TIMERCNT		0x0048
#define RE_EECMD		0x0050
#define RE_CFG0		0x0051
#define RE_CFG1		0x0052
#define RE_CFG2		0x0053
#define RE_CFG3		0x0054
#define RE_CFG4		0x0055
#define RE_CFG5		0x0056
#define RE_TDFNR		0x0057
#define RE_TIMERINT0		0x0058
#define RE_TIMERINT1		0x005c
#define RE_CSIDR		0x0064
#define RE_CSIAR		0x0068
#define RE_PHYSTAT		0x006c
#define RE_PMCH		0x006f
#define RE_INT_CFG1		0x007a
#define RE_EPHYAR		0x0080
#define RE_TIMERINT2		0x008c
#define RE_TXSTART		0x0090
#define RE_MACOCP		0x00b0
#define RE_PHYOCP		0x00b8
#define RE_DLLPR		0x00d0
#define RE_TWICMD		0x00d2
#define RE_MCUCMD		0x00d3
#define RE_RXMAXSIZE		0x00da
#define RE_CPLUSCMD		0x00e0
#define RE_IM			0x00e2
#define RE_RXDESC_ADDR_LO	0x00e4
#define RE_RXDESC_ADDR_HI	0x00e8
#define RE_PPSW		0x00f2
#define RE_TIMERINT3		0x00f4
#define RE_RADMFIFO_PROTECT	0x0402
#define RE_INTMITI(i)		(0x0a00 + (i) * 4)
#define RE_PHYBASE		0x0a40
#define RE_EPHYAR_EXT_ADDR	0x0ffe
#define RE_ADDR0		0x19e0
#define RE_ADDR1		0x19e4
#define RE_RSS_CTRL		0x4500
#define RE_RXQUEUE_CTRL	0x4800
#define RE_EEE_TXIDLE_TIMER	0x6048

/* Flags for register RE_INT_CFG0 */
#define RE_INT_CFG0_EN			0x01
#define RE_INT_CFG0_TIMEOUT_BYPASS	0x02
#define RE_INT_CFG0_MITIGATION_BYPASS	0x04
#define RE_INT_CFG0_RDU_BYPASS_8126	0x10
#define RE_INT_CFG0_AVOID_MISS_INTR	0x40
#define RE_INT_CFG0_MSIX_ENTRY_NUM_MODE (1 << 5)
#define RE_INT_CFG0_AUTO_CLEAR_IMR (1 << 5)
/* Flags for register RE_CMD */
#define RE_CMD_RXBUF_EMPTY	0x01
#define RE_CMD_TXENB		0x04
#define RE_CMD_RXENB		0x08
#define RE_CMD_RESET		0x10
#define RE_CMD_STOPREQ		0x80

/* Flags for register RE_ISR */
#define RE_ISR_RX_OK		0x00000001
#define RE_ISR_RX_ERR		0x00000002
#define RE_ISR_TX_OK		0x00000004
#define RE_ISR_TX_ERR		0x00000008
#define RE_ISR_RX_DESC_UNAVAIL	0x00000010
#define RE_ISR_LINKCHG		0x00000020
#define RE_ISR_RX_FIFO_OFLOW	0x00000040
#define RE_ISR_TX_DESC_UNAVAIL	0x00000080
#define RE_ISR_SWI		0x00000100
#define RE_ISR_PCS_TIMEOUT	0x00004000
#define RE_ISR_SYSTEM_ERR	0x00008000

#define RE_INTRS		\
	(RE_ISR_RX_OK | RE_ISR_RX_ERR | RE_ISR_TX_OK |		\
	RE_ISR_TX_ERR | RE_ISR_LINKCHG | RE_ISR_TX_DESC_UNAVAIL |	\
	RE_ISR_PCS_TIMEOUT | RE_ISR_SYSTEM_ERR)

#define RE_INTRS_TIMER		\
	(RE_ISR_RX_ERR | RE_ISR_TX_ERR | RE_ISR_PCS_TIMEOUT |	\
	RE_ISR_SYSTEM_ERR)

/* v2 */
#define RE_8125B_ISR_RXQ0_OK	0x00000001
#define RE_8125B_ISR_TXQ0_OK	0x00010000
#define RE_8125B_ISR_TXQ1_OK	0x00040000
#define RE_8125B_ISR_LINKCHG	0x00200000
#define RE_INTRS_8125B	\
	(RE_8125B_ISR_RXQ0_OK|RE_8125B_ISR_TXQ0_OK)

/* v4*/
#define RE_8126_ISR_TRXQ0_OK	0x00000001
#define RE_8126_ISR_LINKCHG	0x00200000
#define RE_INTRS_8126	\
	(RE_8126_ISR_TRXQ0_OK)

/* v4 */
#define RE_8125BP_ISR_TRXQ0_OK	0x00000001
#define RE_8125BP_ISR_LINKCHG	0x20000000
#define RE_INTRS_8125BP	\
	(RE_8125BP_ISR_TRXQ0_OK)
/* v7 */
#define RE_8125CP_ISR_RXQ0_OK	0x00000001
#define RE_8125CP_ISR_TXQ0_OK	0x08000000
#define RE_8125CP_ISR_TXQ1_OK	0x10000000
#define RE_8125CP_ISR_LINKCHG	0x20000000
#define RE_INTRS_8125CP	\
	(RE_8125CP_ISR_RXQ0_OK|RE_8125CP_ISR_TXQ0_OK)

/* v5 */
#define RE_8125D_ISR_RXQ0_OK	0x00000001
#define RE_8125D_ISR_TXQ0_OK	0x00010000
#define RE_8125D_ISR_TXQ1_OK	0x00020000
#define RE_8125D_ISR_LINKCHG	0x00040000
#define RE_INTRS_8125D	\
	(RE_8125D_ISR_RXQ0_OK|RE_8125D_ISR_TXQ0_OK)

/* v6 */
#define RE_8127_ISR_RXQ0_OK	0x00000001
#define RE_8127_ISR_TXQ0_OK	0x00000100
#define RE_8127_ISR_TXQ1_OK	0x00000200
#define RE_8127_ISR_LINKCHG	0x20000000
#define RE_8127_L2_ISR		0x80000000
#define RE_INTRS_8127	\
	(RE_8127_ISR_RXQ0_OK|RE_8127_ISR_TXQ0_OK)

/* Flags for register RE_TXCFG */
#define RE_TXCFG_HWREV		0x7cf00000

/* Flags for register RE_RXCFG */
#define RE_RXCFG_ALLPHYS	0x00000001
#define RE_RXCFG_INDIV		0x00000002
#define RE_RXCFG_MULTI		0x00000004
#define RE_RXCFG_BROAD		0x00000008
#define RE_RXCFG_RUNT		0x00000010
#define RE_RXCFG_ERRPKT	0x00000020
#define RE_RXCFG_VLANSTRIP	0x00c00000

/* Flags for register RE_EECMD */
#define RE_EECMD_WRITECFG	0xc0

/* Flags for register RE_CFG1 */
#define RE_CFG1_PM_EN		0x01
#define RE_CFG1_SPEED_DOWN	0x10

/* Flags for register RE_CFG2 */
#define RE_CFG2_PMSTS_EN	0x20
#define RE_CFG2_CLKREQ_EN	0x80

/* Flags for register RE_CFG3 */
#define RE_CFG3_RDY_TO_L23	0x02
#define RE_CFG3_WOL_LINK	0x10
#define RE_CFG3_WOL_MAGIC	0x20

/* Flags for register RE_CFG5 */
#define RE_CFG5_PME_STS	0x01
#define RE_CFG5_WOL_LANWAKE	0x02
#define RE_CFG5_WOL_UCAST	0x10
#define RE_CFG5_WOL_MCAST	0x20
#define RE_CFG5_WOL_BCAST	0x40

/* Flags for register RE_CSIAR */
#define RE_CSIAR_BYTE_EN	0x0000000f
#define RE_CSIAR_BYTE_EN_SHIFT	12
#define RE_CSIAR_ADDR_MASK	0x00000fff
#define RE_CSIAR_BUSY		0x80000000

/* Flags for register RE_PHYSTAT */
#define RE_PHYSTAT_FDX		0x0001
#define RE_PHYSTAT_LINK	0x0002
#define RE_PHYSTAT_10MBPS	0x0004
#define RE_PHYSTAT_100MBPS	0x0008
#define RE_PHYSTAT_1000MBPS	0x0010
#define RE_PHYSTAT_RXFLOW	0x0020
#define RE_PHYSTAT_TXFLOW	0x0040
#define RE_PHYSTAT_2500MBPS	0x0400
#define RE_PHYSTAT_5000MBPS	0x1000
#define RE_PHYSTAT_10000MBPS	0x4000

/* Flags for register RE_EPHYAR */
#define RE_EPHYAR_DATA_MASK	0x0000ffff
#define RE_EPHYAR_BUSY		0x80000000
#define RE_EPHYAR_ADDR_MASK	0x0000007f
#define RE_EPHYAR_ADDR_SHIFT	16

/* Flags for register RE_TXSTART */
#define RE_TXSTART_START	0x0001

/* Flags for register RE_MACOCP */
#define RE_MACOCP_DATA_MASK	0x0000ffff
#define RE_MACOCP_BUSY		0x80000000
#define RE_MACOCP_ADDR_SHIFT	16

/* Flags for register RE_PHYOCP */
#define RE_PHYOCP_DATA_MASK	0x0000ffff
#define RE_PHYOCP_BUSY		0x80000000
#define RE_PHYOCP_ADDR_SHIFT	16

/* Flags for register RE_DLLPR. */
#define RE_DLLPR_PFM_EN	0x40
#define RE_DLLPR_TX_10M_PS_EN	0x80

/* Flags for register RE_MCUCMD */
#define RE_MCUCMD_RXFIFO_EMPTY	0x10
#define RE_MCUCMD_TXFIFO_EMPTY	0x20
#define RE_MCUCMD_IS_OOB	0x80

/* Flags for register RE_CPLUSCMD */
#define RE_CPLUSCMD_RXCSUM	0x0020

#define RE_TX_NSEGS		32

#define RE_TX_LIST_CNT		1024
#define RE_RX_LIST_CNT		1024

#define RE_ALIGN		256
#define RE_TX_LIST_SZ		(sizeof(struct re_tx_desc) * RE_TX_LIST_CNT)
#define RE_RX_LIST_SZ		(sizeof(struct re_rx_desc) * RE_RX_LIST_CNT)
#define RE_NEXT_TX_DESC(x)	(((x) + 1) % RE_TX_LIST_CNT)
#define RE_NEXT_RX_DESC(x)	(((x) + 1) % RE_RX_LIST_CNT)
#define RE_ADDR_LO(y)		((uint64_t) (y) & 0xffffffff)
#define RE_ADDR_HI(y)		((uint64_t) (y) >> 32)

#define RE_ADV_2500TFDX	0x0080
#define RE_ADV_5000TFDX	0x0100
#define RE_ADV_10000TFDX	0x1000

/* Tx descriptor */
struct re_tx_desc {
	uint32_t		re_cmdsts;
	uint32_t		re_extsts;
	uint64_t		re_addr;
	uint32_t		reserved[4];
} __packed __aligned(16);

#define RE_TDCMDSTS_COLL	0x000f0000
#define RE_TDCMDSTS_EXCESSCOLL	0x00100000
#define RE_TDCMDSTS_TXERR	0x00800000
#define RE_TDCMDSTS_EOF	0x10000000
#define RE_TDCMDSTS_SOF	0x20000000
#define RE_TDCMDSTS_EOR	0x40000000
#define RE_TDCMDSTS_OWN	0x80000000

#define RE_TDEXTSTS_VTAG	0x00020000
#define RE_TDEXTSTS_IPCSUM	0x20000000
#define RE_TDEXTSTS_TCPCSUM	0x40000000
#define RE_TDEXTSTS_UDPCSUM	0x80000000

#define RE_RDCMDSTS_RXERRSUM	0x00100000
#define RE_RDCMDSTS_EOF	0x01000000
#define RE_RDCMDSTS_SOF	0x02000000
#define RE_RDCMDSTS_EOR	0x40000000
#define RE_RDCMDSTS_OWN	0x80000000
#define RE_RDCMDSTS_FRAGLEN	0x00003fff

#define RE_RDEXTSTS_VTAG	0x00010000
#define RE_RDEXTSTS_VLAN_MASK	0x0000ffff
#define RE_RDEXTSTS_TCPCSUMERR	0x01000000
#define RE_RDEXTSTS_UDPCSUMERR	0x02000000
#define RE_RDEXTSTS_IPCSUMERR	0x04000000
#define RE_RDEXTSTS_TCPPKT	0x10000000
#define RE_RDEXTSTS_UDPPKT	0x20000000
#define RE_RDEXTSTS_IPV4	0x40000000
#define RE_RDEXTSTS_IPV6	0x80000000

/*
 * @brief Statistics counter structure
 *
 * This is the layout of the hardware structure that
 * is populated by the hardware when RE_DTCCR_* is
 * appropriately poked.
 */
struct re_stats {
	uint64_t re_tx_pkts;
	uint64_t re_rx_pkts;
	uint64_t re_tx_errs;
	uint32_t re_rx_errs;
	uint16_t re_missed_pkts;
	uint16_t re_rx_framealign_errs;
	uint32_t re_tx_onecoll;
	uint32_t re_tx_multicolls;
	uint64_t re_rx_ucasts;
	uint64_t re_rx_bcasts;
	uint32_t re_rx_mcasts;
	uint16_t re_tx_aborts;
	uint16_t re_rx_underruns;

	/* extended */
	uint64_t re_tx_octets;
	uint64_t re_rx_octets;
	uint64_t re_rx_multicast64;
	uint64_t re_tx_unicast64;
	uint64_t re_tx_broadcast64;
	uint64_t re_tx_multicast64;
	uint32_t re_tx_pause_on;
	uint32_t re_tx_pause_off;
	uint32_t re_tx_pause_all;
	uint32_t re_tx_deferred;
	uint32_t re_tx_late_collision;
	uint32_t re_tx_all_collision;
	uint32_t re_tx_aborted32;
	uint32_t re_align_errors32;
	uint32_t re_rx_frame_too_long;
	uint32_t re_rx_runt;
	uint32_t re_rx_pause_on;
	uint32_t re_rx_pause_off;
	uint32_t re_rx_pause_all;
	uint32_t re_rx_unknown_opcode;
	uint32_t re_rx_mac_error;
	uint32_t re_tx_underrun32;
	uint32_t re_rx_mac_missed;
	uint32_t re_rx_tcam_dropped;
	uint32_t re_tdu;
	uint32_t re_rdu;
};


#define RE_STATS_BUF_SIZE	sizeof(struct re_stats)

#define RE_STATS_ALIGNMENT	64

/* Ram version */
#define RE_MAC_R25D_1_RCODE_VER	0x0027
#define RE_MAC_R25D_2_RCODE_VER	0x0031
#define RE_MAC_R26_1_RCODE_VER		0x0033
#define RE_MAC_R26_2_RCODE_VER		0x0060
#define RE_MAC_R27_RCODE_VER		0x0036
#define RE_MAC_R25_RCODE_VER		0x0b33
#define RE_MAC_R25B_RCODE_VER		0x0b99

#define RE_TYPE_R25D(sc)						\
	((sc)->re_type == MAC_R25D_1 || (sc)->re_type == MAC_R25D_2)

#define RE_TYPE_R26(sc)						\
	((sc)->re_type == MAC_R26_1 || (sc)->re_type == MAC_R26_2)

#define RE_TIMEOUT		100

#define	ETHER_VLAN_ENCAP_LEN	4
#define	ETHER_CRC_LEN		4
#define	ETHER_ADDR_LEN		6
#define	ETHER_TYPE_LEN		2
#define	ETHER_HDR_LEN		14

#define RE_JUMBO_FRAMELEN	9216
#define RE_JUMBO_MTU							\
	(RE_JUMBO_FRAMELEN - ETHER_HDR_LEN - ETHER_CRC_LEN - 		\
	ETHER_VLAN_ENCAP_LEN)

#define RE_TXCFG_CONFIG	0x03000700
#define RE_RXCFG_CONFIG	0x41000700
#define RE_RXCFG_CONFIG_8125B	0x41000c00
#define RE_RXCFG_CONFIG_8125D	0x41200c00
#define RE_RXCFG_CONFIG_8126	0x41200d00


/* new regs what not exist in original driver,
 * if it will be added in new driver versions,
 * replace them and remove.
 */
#define LEDSEL_1_8125   0x0086
#define LEDSEL_2_8125   0x0084
#define LEDSEL_3_8125   0x0096
#define TCTR0_8125   0x0048
#define TCTR1_8125 	 0x004C
#define TCTR2_8125   0x0088
#define TCTR3_8125   0x001C
#define TIMER_INT0_8125     0x0058
#define TIMER_INT1_8125     0x005C
#define TIMER_INT2_8125     0x008C
#define TIMER_INT3_8125     0x00F4
#define SW_TAIL_PTR0_8125   0x2800
#define HW_CLO_PTR0_8125    0x2802
#define SW_TAIL_PTR0_8126   0x2800
#define HW_CLO_PTR0_8126    0x2800
#define RDSAR_Q1_LOW_8125   0x4000
#define RSS_KEY_8125    0x4600
#define RSS_INDIRECTION_TBL_8125_V2 0x4700
#define RE_RSS_KEY_SIZE     40  /* size of RSS Hash Key in bytes */
#define RE_MAX_INDIRECTION_TABLE_ENTRIES 128
#define R8125_PHY_FUSE_DOUT_NUM (32)
#define R8125_MAX_PHY_FUSE_DOUT_NUM R8125_PHY_FUSE_DOUT_NUM

#define RE_ISR_SW_INT 0x0100

#define RE_TX_UDPCS_C   0x80000000
#define RE_TX_TCPCS_C   0x40000000
#define RE_TX_IPCS_C    0x20000000
#define RE_TX_IPV6F_C   0x10000000

#define RE_IMR_V2_CLEAR_REG_8125 0x0D00
#define RE_IMR_V2_SET_REG_8125 0x0D0C
#define RE_ISR_V2_8125 0x0D04
#define RE_IMR_V4_L2_CLEAR_REG_8125 0x0D10
#define RE_IMR_V4_L2_SET_REG_8125 0x0D18
#define RE_ISR_V4_L2_8125 0x0D14
#define SW_TAIL_PTR0_8125BP  0x0D30
#define SW_TAIL_PTR1_8125BP  0x0D38
#define HW_CLO_PTR0_8125BP  0x0D34
#define HW_CLO_PTR1_8125BP  0x0D3C
#define RE_INT_MITI_V2_0_RX 0x0A00
#define RE_INT_MITI_V2_0_TX 0x0A02
#define RE_INT_MITI_BASE 0x0A00
#define RE_INT_MITI_END_V3 0x0B00
#define RE_INT_MITI_END_V2 0x0A80

/* rss */
#define RE_RSS_KEY_SIZE 40
#define RE_MAX_INDIRECTION_TABLE_ENTRIES 128

#define RE_RSS_FLAG_HASH_UDP_IPV4  (1U << 0)
#define RE_RSS_FLAG_HASH_UDP_IPV6  (1U << 1)

#define RE_RSS_MASK_BITS_OFFSET 8
#define RE_RSS_CPU_NUM_OFFSET   16

#define RE_RXS_RSS_UDP_V3       0x00000200
#define RE_RXS_RSS_IPV4_V3      0x00000400
#define RE_RXS_RSS_IPV6_V3      0x00001000
#define RE_RXS_RSS_TCP_V3       0x00002000

#define RE_RXS_RSS_UDP_V4       0x08000000
#define RE_RXS_RSS_IPV4_V4      0x10000000
#define RE_RXS_RSS_IPV6_V4      0x20000000
#define RE_RXS_RSS_TCP_V4       0x40000000

#define RE_RXS_RSS_L3_MASK_V3   (RE_RXS_RSS_IPV4_V3 | RE_RXS_RSS_IPV6_V3)
#define RE_RXS_RSS_L4_MASK_V3   (RE_RXS_RSS_TCP_V3  | RE_RXS_RSS_UDP_V3)

#define RE_RXS_RSS_L3_MASK_V4   (RE_RXS_RSS_IPV4_V4 | RE_RXS_RSS_IPV6_V4)
#define RE_RXS_RSS_L4_MASK_V4   (RE_RXS_RSS_TCP_V4  | RE_RXS_RSS_UDP_V4)

enum re_rss_register_content {
    RE_RSS_CTRL_TCP_IPV4_SUPP      = 0x00000001,
    RE_RSS_CTRL_IPV4_SUPP          = 0x00000002,
    RE_RSS_CTRL_TCP_IPV6_SUPP      = 0x00000004,
    RE_RSS_CTRL_IPV6_SUPP          = 0x00000008,
    RE_RSS_CTRL_IPV6_EXT_SUPP      = 0x00000010,
    RE_RSS_CTRL_TCP_IPV6_EXT_SUPP  = 0x00000020,
    RE_RSS_HALF_SUPP               = 0x00000080,
    RE_RSS_CTRL_UDP_IPV4_SUPP      = 0x00000800,
    RE_RSS_CTRL_UDP_IPV6_SUPP      = 0x00001000,
    RE_RSS_CTRL_UDP_IPV6_EXT_SUPP  = 0x00002000,
    RE_RSS_QUAD_CPU_EN             = 0x00010000,
    RE_RSS_HQ_Q_SUP_R              = 0x80000000,
};

#define DescOwn 1 << 31
#define RingEnd 1 << 30
#define FirstFrag 1 << 29
#define LastFrag 1 << 28

#define DescOwn_V3 DescOwn
#define RingEnd_V3 RingEnd
#define FirstFrag_V3 1 << 25
#define LastFrag_V3 1 << 24

#define DescOwn_V4 DescOwn
#define RingEnd_V4 RingEnd
#define FirstFrag_V4 FirstFrag
#define LastFrag_V4 LastFrag

#define GiantSendv4 1 << 26
#define GiantSendv6 1 << 25

#define EnableRxDescV3 (1 << 24)
#define EnableRxDescV4_1 (1 << 24)
#define EnableRxDescV4_0 (1 << 1)

#define RL_PHY_STATUS_500MF 0x80000
#define RL_PHY_STATUS_10000MF 0x4000
#define RL_PHY_STATUS_10000MF_LITE 0x2000
#define RL_PHY_STATUS_5000MF 0x1000
#define RL_PHY_STATUS_5000MF_LITE 0x800
#define RL_PHY_STATUS_2500MF 0x400
#define RL_PHY_STATUS_1250MF 0x200
#define RL_PHY_STATUS_CABLE_PLUG 0x80
#define RL_PHY_STATUS_TX_FLOW_CTRL 0x40
#define RL_PHY_STATUS_RX_FLOW_CTRL 0x20
#define RL_PHY_STATUS_1000MF    0x10
#define RL_PHY_STATUS_100M      0x08
#define RL_PHY_STATUS_10M       0x04
#define RL_PHY_STATUS_LINK_STS  0x02
#define RL_PHY_STATUS_FULL_DUP  0x01

#define RE_MIN_FRAMELEN		60

#define RE_RX_PROTO_UDP            0x20000000
#define RE_RX_PROTO_TCP            0x10000000
#define RE_RX_PROTO_IP             0x30000000
#define RE_RX_PROTO_MASK           0x30000000

#define RE_RX_IPF                  0x00010000
#define RE_RX_UDPF                 0x00008000
#define RE_RX_TCPF                 0x00004000
#define RE_RX_VLAN_TAG             0x00010000
#define RE_RX_UDPT                 0x00040000
#define RE_RX_TCPT                 0x00020000
#define RE_RX_V6F                  0x80000000
#define RE_RX_V4F                  0x40000000

#define RE_RX_PID1_V3             0x20000000
#define RE_RX_PID0_V3             0x10000000
#define RE_RX_PROTO_UDP_V3        0x20000000
#define RE_RX_PROTO_TCP_V3        0x10000000
#define RE_RX_PROTO_IP_V3         0x30000000
#define RE_RX_PROTO_MASK_V3       0x30000000

#define RE_RX_IPF_V3              0x04000000
#define RE_RX_UDPF_V3             0x02000000
#define RE_RX_TCPF_V3             0x01000000
#define RE_RX_SCTPF_V3            0x00800000
#define RE_RX_VLAN_TAG_V3         0x00010000
#define RE_RX_UDPT_V3             0x20000000
#define RE_RX_TCPT_V3             0x10000000
#define RE_RX_SCTP_V3             0x08000000
#define RE_RX_V6F_V3              0x80000000
#define RE_RX_V4F_V3              0x40000000

#define RE_RX_IPF_V4              0x00020000
#define RE_RX_UDPF_V4             0x00010000
#define RE_RX_TCPF_V4             0x00008000
#define RE_RX_SCTPF_V4            0x00080000
#define RE_RX_VLAN_TAG_V4         0x00010000
#define RE_RX_UDPT_V4             0x00080000
#define RE_RX_TCPT_V4             0x00040000
#define RE_RX_SCTP_V4             0x00080000
#define RE_RX_V6F_V4              0x80000000
#define RE_RX_V4F_V4              0x40000000

#endif	/* __IF_REREG_H__ */
