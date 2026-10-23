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

static const ddi_device_acc_attr_t re_reg_map_accattr = {
	DDI_DEVICE_ATTR_V1,
	DDI_STRUCTURE_LE_ACC,
	DDI_STRICTORDER_ACC,
	DDI_DEFAULT_ACC
};

static const ddi_device_acc_attr_t re_stat_accattr = {
	DDI_DEVICE_ATTR_V1,
	DDI_NEVERSWAP_ACC,
	DDI_STRICTORDER_ACC,
	DDI_DEFAULT_ACC
};

static const ddi_dma_attr_t re_stats_dma_attr = {
	.dma_attr_version	= DMA_ATTR_V0,
	.dma_attr_addr_lo	= 0,
	.dma_attr_addr_hi	= UINT64_MAX,
	.dma_attr_count_max	= sizeof (struct re_stats),
	.dma_attr_align		= RE_STATS_ALIGNMENT,
	.dma_attr_burstsizes	= 0xFFFFFFFF,
	.dma_attr_minxfer	= 1,
	.dma_attr_maxxfer	= sizeof (struct re_stats),
	.dma_attr_seg		= UINT64_MAX,
	.dma_attr_sgllen	= 1,
	.dma_attr_granular	= 1,
	.dma_attr_flags		= 0
};

static char force_msi_propname[] = "force_msi";
static char timer_int_enable_propname[] = "timer_int_enable";
static char timer_count_propname[] = "timer_count";

static void
re_pci_cfg_init(struct re_softc *sc)
{
	ddi_acc_handle_t h = sc->cfg_handle;

	sc->pci_cfg.command = pci_config_get16(h, PCI_CONF_COMM);
	sc->pci_cfg.vendor = pci_config_get16(h, PCI_CONF_VENID);
	sc->pci_cfg.subven = pci_config_get16(h, PCI_CONF_SUBVENID);
	sc->pci_cfg.subdev = pci_config_get16(h, PCI_CONF_SUBSYSID);
	sc->pci_cfg.device = pci_config_get16(h, PCI_CONF_DEVID);
	sc->pci_cfg.revision = pci_config_get8(h, PCI_CONF_REVID);
	sc->pci_cfg.clsize = pci_config_get8(h, PCI_CONF_CACHE_LINESZ);
	sc->pci_cfg.ilr = pci_config_get8(h, PCI_CONF_ILINE);
	sc->re_device_id = sc->pci_cfg.device;

	sc->pci_cfg.command |= PCI_COMM_ME | PCI_COMM_MAE;
	pci_config_put16(h, PCI_CONF_COMM, sc->pci_cfg.command);
}

static int
re_alloc_stats(struct re_softc *sc)
{
	int err;
	re_dma_buffer_t *dma_p = &sc->re_tally.tally_buf;

	dma_p->re_db_va = NULL;
	dma_p->re_acc_hdl = NULL;
	dma_p->re_dma_hdl = NULL;
	dma_p->ncookies = 0;
	dma_p->re_db_alloc_len = 0;

	err = ddi_dma_alloc_handle(sc->dev, &re_stats_dma_attr,
	    DDI_DMA_SLEEP, NULL, &dma_p->re_dma_hdl);
	if (err != DDI_SUCCESS)
		return (DDI_FAILURE);

	err = ddi_dma_mem_alloc(dma_p->re_dma_hdl, RE_STATS_BUF_SIZE,
	    &re_stat_accattr, DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL,
	    &dma_p->re_db_va, &dma_p->re_db_alloc_len, &dma_p->re_acc_hdl);
	if (err != DDI_SUCCESS) {
		ddi_dma_free_handle(&dma_p->re_dma_hdl);
		dma_p->re_dma_hdl = NULL;
		return (DDI_FAILURE);
	}

	err = ddi_dma_addr_bind_handle(dma_p->re_dma_hdl, NULL,
	    dma_p->re_db_va, dma_p->re_db_alloc_len,
	    DDI_DMA_RDWR | DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL,
	    &dma_p->cookie, &dma_p->ncookies);

	if (err != DDI_DMA_MAPPED || dma_p->ncookies != 1) {
		if (err == DDI_DMA_MAPPED)
			(void) ddi_dma_unbind_handle(dma_p->re_dma_hdl);

		ddi_dma_mem_free(&dma_p->re_acc_hdl);
		dma_p->re_acc_hdl = NULL;
		dma_p->re_db_va = NULL;

		ddi_dma_free_handle(&dma_p->re_dma_hdl);
		dma_p->re_dma_hdl = NULL;

		return (DDI_FAILURE);
	}

	sc->re_tally.stats = (struct re_stats *)(void *)dma_p->re_db_va;
	bzero(sc->re_tally.stats, sizeof (struct re_stats));

	return (DDI_SUCCESS);
}

static void
re_free_stats(struct re_softc *sc)
{
	re_dma_buffer_t *dma_p = &sc->re_tally.tally_buf;

	if (dma_p->re_dma_hdl != NULL) {
		if (dma_p->ncookies != 0) {
			(void) ddi_dma_unbind_handle(dma_p->re_dma_hdl);
			dma_p->ncookies = 0;
			bzero(&dma_p->cookie, sizeof (dma_p->cookie));
		}
	}

	if (dma_p->re_acc_hdl != NULL) {
		ddi_dma_mem_free(&dma_p->re_acc_hdl);
		dma_p->re_acc_hdl = NULL;
	}
	dma_p->re_db_va = NULL;
	dma_p->re_db_alloc_len = 0;

	if (dma_p->re_dma_hdl != NULL) {
		ddi_dma_free_handle(&dma_p->re_dma_hdl);
		dma_p->re_dma_hdl = NULL;
	}

	sc->re_tally.stats = NULL;
}

static int
re_reinit_rings_for_reset(struct re_softc *sc)
{
	int i;

	for (i = 0; i < sc->num_tx_rings; i++) {
		re_tx_ring_t *tr = &sc->tx_rings[i];

		tr->cur_tx = 0;
		tr->dirty_tx = 0;

		re_tx_desc_init_ring(tr);
	}

	for (i = 0; i < sc->num_rx_rings; i++) {
		re_rx_ring_t *rr = &sc->rx_rings[i];

		if (re_rx_ring_fill(sc, rr) != DDI_SUCCESS)
			return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

static void
re_reset_task(void *arg)
{
	struct re_softc *sc = arg;

	mutex_enter(&sc->mtx);
	sc->re_reset_counter++;

	if (sc->suspended ||
	    ((sc->attach_state & RE_ATTACH_MAC_START) == 0)) {
		sc->reset_pending = B_FALSE;
		mutex_exit(&sc->mtx);
		return;
	}

	if (sc->reset_running) {
		mutex_exit(&sc->mtx);
		return;
	}

	sc->reset_pending = B_FALSE;
	sc->reset_running = B_TRUE;
	dev_err(sc->dev, CE_WARN,
	    "reset task running, reason=0x%x", sc->reset_reason);
	re_stop_locked(sc);

	if (re_reinit_rings_for_reset(sc) != DDI_SUCCESS) {
		sc->sc_stopped = B_TRUE;
		sc->reset_running = B_FALSE;
		mutex_exit(&sc->mtx);
		return;
	}

	re_init_locked(sc);

	sc->reset_running = B_FALSE;

	mutex_exit(&sc->mtx);
}

void
re_schedule_reset_locked(struct re_softc *sc, uint32_t reason)
{
	RE_LOCK_ASSERT(sc);

	if (sc->reset_pending || sc->reset_running)
		return;

	sc->reset_pending = B_TRUE;
	sc->reset_reason = reason;

	if (ddi_taskq_dispatch(sc->reset_tq, re_reset_task, sc,
	    DDI_NOSLEEP) == DDI_FAILURE) {
		sc->reset_pending = B_FALSE;
	}
}

static boolean_t
re_tx_ring_has_pending(re_tx_ring_t *tr)
{
	uint32_t i;
	boolean_t pending = B_FALSE;

	mutex_enter(&tr->tx_lock);

	for (i = 0; i < tr->num_desc; i++) {
		if (tr->tx_work_list[i] != NULL) {
			pending = B_TRUE;
			break;
		}
	}

	mutex_exit(&tr->tx_lock);

	return (pending);
}

#define	RE_TX_WATCHDOG_NS	(6LL * NANOSEC)

static void
re_watchdog_periodic(void *arg)
{
	struct re_softc *sc = arg;
	hrtime_t now;
	int i;

	mutex_enter(&sc->mtx);

	if (sc->suspended ||
	    sc->reset_pending ||
	    sc->reset_running ||
	    ((sc->attach_state & RE_ATTACH_MAC_START) == 0)) {
		mutex_exit(&sc->mtx);
		return;
	}

	now = gethrtime();

	for (i = 0; i < sc->num_tx_rings; i++) {
		re_tx_ring_t *tr = &sc->tx_rings[i];
		hrtime_t last;

		if (!re_tx_ring_has_pending(tr))
			continue;

		last = tr->tx_last_reclaim_time;

		if (last == 0)
			last = tr->tx_last_submit_time;

		if (last == 0)
			continue;

		if (now - last > RE_TX_WATCHDOG_NS) {
			tr->tx_watchdog_timeouts++;
			re_schedule_reset_locked(sc, RE_RESET_TX_TIMEOUT);
			break;
		}
	}

	mutex_exit(&sc->mtx);
}

static void
re_unattach(struct re_softc *sc)
{
	sc->sc_stopped = B_TRUE;

	if ((sc->attach_state & RE_ATTACH_REGS_MAP) != 0)
		re_disable_extend_tally_couter(sc);

	if (sc->link_status != NULL) {
		ddi_periodic_delete(sc->link_status);
		sc->link_status = NULL;
	}

	if (sc->watchdog_periodic != NULL) {
		ddi_periodic_delete(sc->watchdog_periodic);
		sc->watchdog_periodic = NULL;
	}

	if (sc->reset_tq != NULL) {
		ddi_taskq_destroy(sc->reset_tq);
		sc->reset_tq = NULL;
	}

	if ((sc->attach_state & RE_ATTACH_MAC_START) != 0) {
		mutex_enter(&sc->mtx);
		re_stop_locked(sc);
		re_free_rings_data(sc);
		sc->attach_state &= ~RE_ATTACH_MAC_START;
		mutex_exit(&sc->mtx);
	}

	if ((sc->attach_state & RE_ATTACH_MAC) != 0) {
		(void) mac_unregister(sc->mh);
		sc->mh = NULL;
		sc->attach_state &= ~RE_ATTACH_MAC;
	}

	/*
	 * DDI interrupts must be disabled before the handlers are removed.
	 */
	if ((sc->attach_state & RE_ATTACH_ENABLE_INTR) != 0) {
		re_intr_disable_all(sc);
		sc->attach_state &= ~RE_ATTACH_ENABLE_INTR;
	}

	if ((sc->attach_state & RE_ATTACH_ADD_INTR) != 0) {
		re_intr_teardown_handlers(sc);
		sc->attach_state &= ~RE_ATTACH_ADD_INTR;
	}

	if ((sc->attach_state & RE_ATTACH_ALLOC_INTR) != 0) {
		re_free_intr(sc);
		sc->attach_state &= ~RE_ATTACH_ALLOC_INTR;
	}

	if ((sc->attach_state & RE_ATTACH_IRM) != 0) {
		re_irm_unregister(sc);
		sc->attach_state &= ~RE_ATTACH_IRM;
	}

	re_sensor_fini(sc);

	if ((sc->attach_state & RE_ATTACH_ALLOC_RINGS) != 0) {
		re_free_rings(sc);
		sc->attach_state &= ~RE_ATTACH_ALLOC_RINGS;
	}

	if ((sc->attach_state & RE_ATTACH_KSTATS) != 0) {
		re_softc_kstat_fini(sc);
		sc->attach_state &= ~RE_ATTACH_KSTATS;
	}

	if ((sc->attach_state & RE_ATTACH_STATS) != 0) {
		re_free_stats(sc);
		sc->attach_state &= ~RE_ATTACH_STATS;
	}

	if ((sc->attach_state & RE_ATTACH_LOCKS) != 0) {
		mutex_destroy(&sc->mtx);
		sc->attach_state &= ~RE_ATTACH_LOCKS;
	}

	if ((sc->attach_state & RE_ATTACH_REGS_MAP) != 0) {
		ddi_regs_map_free(&sc->io_handle);
		sc->io_regs = NULL;
		sc->attach_state &= ~RE_ATTACH_REGS_MAP;
	}

	if ((sc->attach_state & RE_ATTACH_PCI_CONFIG) != 0) {
		pci_config_teardown(&sc->cfg_handle);
		sc->attach_state &= ~RE_ATTACH_PCI_CONFIG;
	}

	ddi_set_driver_private(sc->dev, NULL);
	kmem_free(sc, sizeof (*sc));
}

static int
re_attach(dev_info_t *devinfo, ddi_attach_cmd_t cmd)
{
	struct re_softc *sc;
	caddr_t regs;
	uint8_t eaddr[ETHERADDRL];
	int err;

	if (cmd != DDI_ATTACH) {
		return (DDI_FAILURE);
	}

	sc = kmem_zalloc(sizeof (*sc), KM_SLEEP);
	ddi_set_driver_private(devinfo, sc);

	sc->force_msi = ddi_prop_get_int(DDI_DEV_T_ANY, devinfo,
	    DDI_PROP_DONTPASS, force_msi_propname, 0);
	sc->timer_int_enable = ddi_prop_get_int(DDI_DEV_T_ANY, devinfo,
	    DDI_PROP_DONTPASS, timer_int_enable_propname, 0);
	sc->timer_count = ddi_prop_get_int(DDI_DEV_T_ANY, devinfo,
	    DDI_PROP_DONTPASS, timer_count_propname, 0x2600);

	sc->dev = devinfo;
	sc->mh = NULL;
	sc->io_regs = NULL;
	sc->mtu = ETHERMTU;
	sc->suspended = 0;
	sc->attach_state = 0;
	sc->re_link_state = LINK_STATE_UNKNOWN;
	sc->re_link_duplex = LINK_DUPLEX_UNKNOWN;
	sc->re_link_speed = 0;

	err = pci_config_setup(devinfo, &sc->cfg_handle);
	if (err != DDI_SUCCESS) {
		dev_err(devinfo, CE_WARN, "pci_config_setup failed");
		goto fail;
	}
	sc->attach_state |= RE_ATTACH_PCI_CONFIG;

	re_pci_cfg_init(sc);

	err = ddi_regs_map_setup(devinfo, 2, &regs, 0, 0,
	    &re_reg_map_accattr, &sc->io_handle);
	if (err != DDI_SUCCESS) {
		dev_err(devinfo, CE_WARN, "ddi_regs_map_setup failed");
		goto fail;
	}
	sc->io_regs = regs;
	sc->attach_state |= RE_ATTACH_REGS_MAP;

	err = re_check_mac_version(sc);
	if (err != DDI_SUCCESS) {
		dev_err(devinfo, CE_WARN, "re_check_mac_version failed");
		goto fail;
	}

	re_init_software_variable(sc);

	/*
	 * Register IRM before interrupt allocation so allocator can account
	 * for desired MSI-X layout immediately.
	 */
	if (sc->msix_min_reqired > 1) {
		err = re_irm_register(sc);
		if (err != DDI_SUCCESS) {
			dev_err(devinfo, CE_WARN, "re_irm_register failed");
			goto fail;
		}
		sc->attach_state |= RE_ATTACH_IRM;
	}

	err = re_alloc_intr(sc);
	if (err != DDI_SUCCESS) {
		dev_err(devinfo, CE_WARN, "re_alloc_intr failed");
		goto fail;
	}
	sc->attach_state |= RE_ATTACH_ALLOC_INTR;

	/*
	 * The softc mutex is taken from the interrupt handler, so it can
	 * only be initialized once the interrupt priority is known.
	 */
	RE_LOCK_INIT(sc);
	sc->attach_state |= RE_ATTACH_LOCKS;

	err = re_alloc_stats(sc);
	if (err != DDI_SUCCESS) {
		dev_err(devinfo, CE_WARN, "re_alloc_stats failed");
		goto fail;
	}
	sc->attach_state |= RE_ATTACH_STATS;

	if (!re_softc_kstat_init(sc)) {
		dev_err(devinfo, CE_WARN, "re_softc_kstat_init failed");
		goto fail;
	}
	sc->attach_state |= RE_ATTACH_KSTATS;

	if (sc->HwCurrIsrVer > 1) {
		if (!(sc->re_intr_type & DDI_INTR_TYPE_MSIX) ||
		    sc->intr_cnt < sc->msix_min_reqired) {
			sc->num_rx_rings = 1;
			sc->num_tx_rings = 1;
			if (sc->timer_int_enable) {
				sc->HwCurrIsrVer = 1;
				sc->use_new_intr_mapping = 0;
			}
			re_setup_interrupt_mask(sc);
		}
	}

	err = re_alloc_rings(sc);
	if (err != DDI_SUCCESS) {
		dev_err(devinfo, CE_WARN, "re_alloc_rings failed");
		goto fail;
	}
	sc->attach_state |= RE_ATTACH_ALLOC_RINGS;

	err = re_intr_setup_handlers(sc);
	if (err != DDI_SUCCESS) {
		dev_err(devinfo, CE_WARN, "re_intr_setup_handlers failed");
		goto fail;
	}
	sc->attach_state |= RE_ATTACH_ADD_INTR;

	err = re_intr_enable_all(sc);
	if (err != DDI_SUCCESS) {
		dev_err(devinfo, CE_WARN, "re_intr_enable_all failed");
		goto fail;
	}
	sc->attach_state |= RE_ATTACH_ENABLE_INTR;

	mutex_enter(&sc->mtx);

	err = re_chipinit(sc);
	if (err != 0) {
		mutex_exit(&sc->mtx);
		dev_err(devinfo, CE_WARN, "re_chipinit failed");
		goto fail;
	}

	re_get_macaddr(sc, eaddr);
	bcopy(eaddr, sc->org_mac_addr, ETHERADDRL);

	mutex_exit(&sc->mtx);

	re_sensor_init(sc);

	/*
	 * ddi_taskq_create() prefixes the name with the driver name and
	 * tags the taskq with the instance number, so the name only has to
	 * be unique within one instance.
	 */
	sc->reset_tq = ddi_taskq_create(sc->dev, "reset", 1,
	    TASKQ_DEFAULTPRI, 0);
	if (sc->reset_tq == NULL) {
		dev_err(devinfo, CE_WARN, "failed to create reset taskq");
		goto fail;
	}

	if (!re_mac_register(sc)) {
		dev_err(devinfo, CE_WARN, "re_mac_register failed");
		goto fail;
	}
	sc->attach_state |= RE_ATTACH_MAC;

	sc->link_status = ddi_periodic_add(re_check_link_status, sc,
	    RE_CYCLIC_PERIOD, DDI_IPL_0);

	sc->watchdog_periodic = ddi_periodic_add(re_watchdog_periodic,
	    sc, RE_CYCLIC_PERIOD, DDI_IPL_0);

	return (DDI_SUCCESS);

fail:
	re_unattach(sc);
	return (DDI_FAILURE);
}

static int
re_detach(dev_info_t *devinfo, ddi_detach_cmd_t cmd)
{
	struct re_softc *sc;

	if (cmd != DDI_DETACH) {
		return (DDI_FAILURE);
	}

	sc = ddi_get_driver_private(devinfo);
	if (sc == NULL)
		return (DDI_FAILURE);

	/*
	 * MAC framework should have stopped us before detach.
	 * Be defensive and refuse detach if runtime start is still active.
	 */
	if ((sc->attach_state & RE_ATTACH_MAC_START) != 0)
		return (DDI_FAILURE);

	re_unattach(sc);
	return (DDI_SUCCESS);
}

/*
 * quiesce(9E) entry point, used by fast reboot.
 *
 * This runs single-threaded with interrupts disabled, so it must not take
 * locks, block or wait for taskqs.  All that is needed is to mask the
 * device interrupts and stop DMA, which re_hw_reset() does by only
 * touching registers and busy-waiting.
 */
static int
re_quiesce(dev_info_t *devinfo)
{
	struct re_softc *sc;

	sc = ddi_get_driver_private(devinfo);
	if (sc == NULL)
		return (DDI_FAILURE);

	if ((sc->attach_state & RE_ATTACH_REGS_MAP) == 0)
		return (DDI_SUCCESS);

	sc->sc_stopped = B_TRUE;
	re_hw_reset(sc);

	return (DDI_SUCCESS);
}

static struct cb_ops re_cb_ops = {
	.cb_open = nulldev,
	.cb_close = nulldev,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = nodev,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_flag = D_MP,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops re_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = NULL,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = re_attach,
	.devo_detach = re_detach,
	.devo_reset = nodev,
	.devo_quiesce = re_quiesce,
	.devo_cb_ops = &re_cb_ops
};

static struct modldrv re_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "Realtek RTL8125/8126/8127 Ethernet Controller",
	.drv_dev_ops = &re_dev_ops
};

static struct modlinkage re_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &re_modldrv, NULL }
};

int
_init(void)
{
	int ret;

	mac_init_ops(&re_dev_ops, RE_MOD_NAME);

	ret = mod_install(&re_modlinkage);
	if (ret != 0)
		mac_fini_ops(&re_dev_ops);

	return (ret);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&re_modlinkage, modinfop));
}

int
_fini(void)
{
	int ret;

	ret = mod_remove(&re_modlinkage);
	if (ret == 0)
		mac_fini_ops(&re_dev_ops);

	return (ret);
}
