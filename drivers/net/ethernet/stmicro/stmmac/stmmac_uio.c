// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2026, Spacemit Corporation.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/uio_driver.h>
#include <linux/list.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/crc32.h>
#include <linux/mii.h>
#include <linux/if.h>
#include <linux/if_vlan.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/prefetch.h>
#include <linux/pinctrl/consumer.h>
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif /* CONFIG_DEBUG_FS */
#include <linux/net_tstamp.h>
#include <linux/udp.h>
#include <net/pkt_cls.h>
#include <net/xdp_sock_drv.h>
#include "stmmac_ptp.h"
#include "stmmac.h"
#include <linux/reset.h>
#include <linux/of_mdio.h>
#include "dwmac1000.h"
#include "dwxgmac2.h"
#include "hwif.h"
#include "mmc.h"

#define DRIVER_NAME	"spacemit_gmac_uio"
#define DRIVER_VERSION	"0.1"

#define STMMAC_UIO_DEVICE_NAME     "stmmac-uio"

#define TC_DEFAULT 64
static int tc = TC_DEFAULT;

#define	DEFAULT_BUFSIZE	1536

enum uio_lifecycle_bits {
	UIO_FLAGS_REMOVED = 0,
};

/* Private data structure */
struct spacemit_gmac_uio_priv {
	struct device *dev;
	struct net_device *ndev;
	char name[32];
	struct uio_info uio;
	int map_num;
	struct stmmac_dma_conf orig_dma_conf;
	atomic_t refcnt;
	unsigned long flags;
};

static int spacemit_uio_takeover_hardware(struct spacemit_gmac_uio_priv *uio_priv);
static void spacemit_uio_restore_hardware(struct spacemit_gmac_uio_priv *uio_priv);

/**
 * spacemit_gmac_uio_open() - UIO file open callback
 * Called by UIO core when userspace executes open("/dev/uioX", ...)
 */
static int spacemit_gmac_uio_open(struct uio_info *info, struct inode *inode)
{
	struct spacemit_gmac_uio_priv *uio_priv = info->priv;

	if (test_bit(UIO_FLAGS_REMOVED, &uio_priv->flags))
		return -ENODEV;

	atomic_inc(&uio_priv->refcnt);
	return 0;
}

/**
 * spacemit_gmac_uio_release() - UIO file close callback
 * Called by UIO core when userspace closes the last fd reference or crashes
 */
static int spacemit_gmac_uio_release(struct uio_info *info,
                		     struct inode *inode)
{
	struct spacemit_gmac_uio_priv *uio_priv = info->priv;
	struct net_device *netdev;

	if (!uio_priv)
		return 0;

	netdev = uio_priv->ndev;

	if (atomic_dec_and_test(&uio_priv->refcnt)) {
		pr_info(DRIVER_NAME ": Final closure from release(): Restoring hardware back to native netdev.\n");
		spacemit_uio_restore_hardware(uio_priv);

		if (netdev)
			dev_put(netdev);

		kfree(uio_priv);
		return 0;
	}

	return 0;
}

static int stmmac_uio_find_mem_index(struct uio_info *info, struct vm_area_struct *vma)
{
	if (vma->vm_pgoff < MAX_UIO_MAPS) {
		if (info->mem[vma->vm_pgoff].size == 0)
			return -1;
		return (int)vma->vm_pgoff;
	}
	return -1;
}

static int spacemit_gmac_uio_mmap(struct uio_info *info,
				  struct vm_area_struct *vma)
{
	int mi;
	struct uio_mem *mem;
	unsigned long requested_pages;
	unsigned long actual_pages;

	if (!info)
		return -EINVAL;

	mi = stmmac_uio_find_mem_index(info, vma);
	if (mi < 0)
		return -EINVAL;

	requested_pages = vma_pages(vma);
	actual_pages = ((info->mem[mi].addr & ~PAGE_MASK)
			+ info->mem[mi].size + PAGE_SIZE -1) >> PAGE_SHIFT;
	if (requested_pages > actual_pages) {
		return -EINVAL;
	}

	mem = info->mem + mi;

	if (mem->addr & ~PAGE_MASK)
		return -ENODEV;
	if (vma->vm_end - vma->vm_start > mem->size)
		return -EINVAL;

	if (mi)
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	else
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);

	return remap_pfn_range(vma,
			       vma->vm_start,
			       mem->addr >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);
}

/**
 * stmmac_uio_init_phy - PHY initialization
 * @dev: net device structure
 * Description: it initializes the driver's PHY state, and attaches the PHY
 * to the mac driver.
 *  Return value:
 *  0 on success
 */
static int stmmac_uio_init_phy(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	struct fwnode_handle *phy_fwnode;
	struct fwnode_handle *fwnode;
	int ret;

	if (!phylink_expects_phy(priv->phylink))
		return 0;

	fwnode = priv->plat->port_node;
	if (!fwnode)
		fwnode = dev_fwnode(priv->device);

	if (fwnode)
		phy_fwnode = fwnode_get_phy_node(fwnode);
	else
		phy_fwnode = NULL;

	/* Some DT bindings do not set-up the PHY handle. Let's try to
	 * manually parse it
	 */
	if (!phy_fwnode || IS_ERR(phy_fwnode)) {
		int addr = priv->plat->phy_addr;
		struct phy_device *phydev;

		if (addr < 0) {
			netdev_err(priv->dev, "no phy found\n");
			return -ENODEV;
		}

		phydev = mdiobus_get_phy(priv->mii, addr);
		if (!phydev) {
			netdev_err(priv->dev, "no phy at addr %d\n", addr);
			return -ENODEV;
		}

		ret = phylink_connect_phy(priv->phylink, phydev);
	} else {
		fwnode_handle_put(phy_fwnode);
		ret = phylink_fwnode_phy_connect(priv->phylink, fwnode, 0);
	}

	if (ret) {
		netdev_err(priv->dev, "cannot attach to PHY (error: %pe)\n",
			   ERR_PTR(ret));
		return ret;
	}

	if (!priv->plat->pmt) {
		struct ethtool_wolinfo wol = { .cmd = ETHTOOL_GWOL };

		phylink_ethtool_get_wol(priv->phylink, &wol);
		device_set_wakeup_capable(priv->device, !!wol.supported);
		device_set_wakeup_enable(priv->device, !!wol.wolopts);
	}

	return 0;
}

static int stmmac_uio_set_bfsize(int mtu, int bufsize)
{
	int ret = bufsize;

	if (mtu >= BUF_SIZE_8KiB)
		ret = BUF_SIZE_16KiB;
	else if (mtu >= BUF_SIZE_4KiB)
		ret = BUF_SIZE_8KiB;
	else if (mtu >= BUF_SIZE_2KiB)
		ret = BUF_SIZE_4KiB;
	else if (mtu > DEFAULT_BUFSIZE)
		ret = BUF_SIZE_2KiB;
	else
		ret = DEFAULT_BUFSIZE;

	return ret;
}

/**
 * __uio_free_dma_rx_desc_resources - free RX dma desc resources (per queue)
 * @priv: private structure
 * @dma_conf: structure to take the dma data
 * @queue: RX queue index
 */
static void __uio_free_dma_rx_desc_resources(struct stmmac_priv *priv,
					     struct stmmac_dma_conf *dma_conf,
					     u32 queue)
{
	struct stmmac_rx_queue *rx_q = &dma_conf->rx_queue[queue];

	/* Free DMA regions of consistent memory previously allocated */
	if (!priv->extend_desc)
		dma_free_coherent(priv->device, dma_conf->dma_rx_size *
				  sizeof(struct dma_desc),
				  rx_q->dma_rx, rx_q->dma_rx_phy);
	else
		dma_free_coherent(priv->device, dma_conf->dma_rx_size *
				  sizeof(struct dma_extended_desc),
				  rx_q->dma_erx, rx_q->dma_rx_phy);
}

static void uio_free_dma_rx_desc_resources(struct stmmac_priv *priv,
					   struct stmmac_dma_conf *dma_conf)
{
	u32 rx_count = priv->plat->rx_queues_to_use;
	u32 queue;

	/* Free RX queue resources */
	for (queue = 0; queue < rx_count; queue++)
		__uio_free_dma_rx_desc_resources(priv, dma_conf, queue);
}

/**
 * __uio_free_dma_tx_desc_resources - free TX dma desc resources (per queue)
 * @priv: private structure
 * @dma_conf: structure to take the dma data
 * @queue: TX queue index
 */
static void __uio_free_dma_tx_desc_resources(struct stmmac_priv *priv,
					     struct stmmac_dma_conf *dma_conf,
					     u32 queue)
{
	struct stmmac_tx_queue *tx_q = &dma_conf->tx_queue[queue];
	size_t size;
	void *addr;

	if (priv->extend_desc) {
		size = sizeof(struct dma_extended_desc);
		addr = tx_q->dma_etx;
	} else if (tx_q->tbs & STMMAC_TBS_AVAIL) {
		size = sizeof(struct dma_edesc);
		addr = tx_q->dma_entx;
	} else {
		size = sizeof(struct dma_desc);
		addr = tx_q->dma_tx;
	}

	size *= dma_conf->dma_tx_size;

	dma_free_coherent(priv->device, size, addr, tx_q->dma_tx_phy);
}

static void uio_free_dma_tx_desc_resources(struct stmmac_priv *priv,
					   struct stmmac_dma_conf *dma_conf)
{
	u32 tx_count = priv->plat->tx_queues_to_use;
	u32 queue;

	/* Free TX queue resources */
	for (queue = 0; queue < tx_count; queue++)
		__uio_free_dma_tx_desc_resources(priv, dma_conf, queue);
}

/**
 * __uio_alloc_dma_rx_desc_resources - alloc RX resources (per queue).
 * @priv: private structure
 * @dma_conf: structure to take the dma data
 * @queue: RX queue index
 * Description: according to which descriptor can be used (extend or basic)
 * this function allocates the resources for TX and RX paths. In case of
 * reception, for example, it pre-allocated the RX socket buffer in order to
 * allow zero-copy mechanism.
 */
static int __uio_alloc_dma_rx_desc_resources(struct stmmac_priv *priv,
					     struct stmmac_dma_conf *dma_conf,
					     u32 queue)
{
	struct stmmac_rx_queue *rx_q = &dma_conf->rx_queue[queue];

	rx_q->queue_index = queue;
	rx_q->priv_data = priv;

	if (priv->extend_desc) {
		rx_q->dma_erx = dma_alloc_coherent(priv->device,
						   dma_conf->dma_rx_size *
						   sizeof(struct dma_extended_desc),
						   &rx_q->dma_rx_phy,
						   GFP_KERNEL);
		if (!rx_q->dma_erx)
			return -ENOMEM;

	} else {
		rx_q->dma_rx = dma_alloc_coherent(priv->device,
						  dma_conf->dma_rx_size *
						  sizeof(struct dma_desc),
						  &rx_q->dma_rx_phy,
						  GFP_KERNEL);
		if (!rx_q->dma_rx)
			return -ENOMEM;
	}

	return 0;
}

static int uio_alloc_dma_rx_desc_resources(struct stmmac_priv *priv,
				       struct stmmac_dma_conf *dma_conf)
{
	u32 rx_count = priv->plat->rx_queues_to_use;
	u32 queue;
	int ret;

	/* RX queues buffers and DMA */
	for (queue = 0; queue < rx_count; queue++) {
		ret = __uio_alloc_dma_rx_desc_resources(priv, dma_conf, queue);
		if (ret)
			goto err_dma;
	}

	return 0;

err_dma:
	uio_free_dma_rx_desc_resources(priv, dma_conf);

	return ret;
}

/**
 * __uio_alloc_dma_tx_desc_resources - alloc TX resources (per queue).
 * @priv: private structure
 * @dma_conf: structure to take the dma data
 * @queue: TX queue index
 * Description: according to which descriptor can be used (extend or basic)
 * this function allocates the resources for TX and RX paths. In case of
 * reception, for example, it pre-allocated the RX socket buffer in order to
 * allow zero-copy mechanism.
 */
static int __uio_alloc_dma_tx_desc_resources(struct stmmac_priv *priv,
					     struct stmmac_dma_conf *dma_conf,
					     u32 queue)
{
	struct stmmac_tx_queue *tx_q = &dma_conf->tx_queue[queue];
	size_t size;
	void *addr;

	tx_q->queue_index = queue;
	tx_q->priv_data = priv;

	if (priv->extend_desc)
		size = sizeof(struct dma_extended_desc);
	else if (tx_q->tbs & STMMAC_TBS_AVAIL)
		size = sizeof(struct dma_edesc);
	else
		size = sizeof(struct dma_desc);

	size *= dma_conf->dma_tx_size;

	addr = dma_alloc_coherent(priv->device, size,
				  &tx_q->dma_tx_phy, GFP_KERNEL);
	if (!addr)
		return -ENOMEM;

	if (priv->extend_desc)
		tx_q->dma_etx = addr;
	else if (tx_q->tbs & STMMAC_TBS_AVAIL)
		tx_q->dma_entx = addr;
	else
		tx_q->dma_tx = addr;

	return 0;
}

static int uio_alloc_dma_tx_desc_resources(struct stmmac_priv *priv,
					   struct stmmac_dma_conf *dma_conf)
{
	u32 tx_count = priv->plat->tx_queues_to_use;
	u32 queue;
	int ret;

	/* TX queues buffers and DMA */
	for (queue = 0; queue < tx_count; queue++) {
		ret = __uio_alloc_dma_tx_desc_resources(priv, dma_conf, queue);
		if (ret)
			goto err_dma;
	}

	return 0;

err_dma:
	uio_free_dma_tx_desc_resources(priv, dma_conf);
	return ret;
}

/**
 * uio_alloc_dma_desc_resources - alloc TX/RX resources.
 * @priv: private structure
 * @dma_conf: structure to take the dma data
 * Description: according to which descriptor can be used (extend or basic)
 * this function allocates the resources for TX and RX paths. In case of
 * reception, for example, it pre-allocated the RX socket buffer in order to
 * allow zero-copy mechanism.
 */
static int uio_alloc_dma_desc_resources(struct stmmac_priv *priv,
					struct stmmac_dma_conf *dma_conf)
{
	/* RX Allocation */
	int ret = uio_alloc_dma_rx_desc_resources(priv, dma_conf);

	if (ret)
		return ret;

	ret = uio_alloc_dma_tx_desc_resources(priv, dma_conf);

	return ret;
}

/**
 * uio_free_dma_desc_resources - free dma desc resources
 * @priv: private structure
 * @dma_conf: structure to take the dma data
 */
static void uio_free_dma_desc_resources(struct stmmac_priv *priv,
					struct stmmac_dma_conf *dma_conf)
{
	/* Release the DMA TX socket buffers */
	uio_free_dma_tx_desc_resources(priv, dma_conf);

	/* Release the DMA RX socket buffers later
	 * to ensure all pending XDP_TX buffers are returned.
	 */
	uio_free_dma_rx_desc_resources(priv, dma_conf);
}

/**
 *  stmmac_uio_mac_enable_rx_queues - Enable MAC rx queues
 *  @priv: driver private structure
 *  Description: It is used for enabling the rx queues in the MAC
 */
static void stmmac_uio_mac_enable_rx_queues(struct stmmac_priv *priv)
{
	u32 rx_queues_count = priv->plat->rx_queues_to_use;
	int queue;
	u8 mode;

	for (queue = 0; queue < rx_queues_count; queue++) {
		mode = priv->plat->rx_queues_cfg[queue].mode_to_use;
		stmmac_rx_queue_enable(priv, priv->hw, mode, queue);
	}
}

/**
 *  stmmac_uio_dma_operation_mode - HW DMA operation mode
 *  @priv: driver private structure
 *  Description: it is used for configuring the DMA operation mode register in
 *  order to program the tx/rx DMA thresholds or Store-And-Forward mode.
 */
static void stmmac_uio_dma_operation_mode(struct stmmac_priv *priv)
{
	u32 rx_channels_count = priv->plat->rx_queues_to_use;
	u32 tx_channels_count = priv->plat->tx_queues_to_use;
	int rxfifosz = priv->plat->rx_fifo_size;
	int txfifosz = priv->plat->tx_fifo_size;
	u32 txmode = 0;
	u32 rxmode = 0;
	u32 chan = 0;
	u8 qmode = 0;

	if (rxfifosz == 0)
		rxfifosz = priv->dma_cap.rx_fifo_size;
	if (txfifosz == 0)
		txfifosz = priv->dma_cap.tx_fifo_size;

	/* Split up the shared Tx/Rx FIFO memory on DW QoS Eth and DW XGMAC */
	if (priv->plat->has_gmac4 || priv->plat->has_xgmac) {
		rxfifosz /= rx_channels_count;
		txfifosz /= tx_channels_count;
	}

	if (priv->plat->force_thresh_dma_mode) {
		txmode = tc;
		rxmode = tc;
	} else if (priv->plat->force_sf_dma_mode || priv->plat->tx_coe) {
		/*
		 * In case of GMAC, SF mode can be enabled
		 * to perform the TX COE in HW. This depends on:
		 * 1) TX COE if actually supported
		 * 2) There is no bugged Jumbo frame support
		 *    that needs to not insert csum in the TDES.
		 */
		txmode = SF_DMA_MODE;
		rxmode = SF_DMA_MODE;
		priv->xstats.threshold = SF_DMA_MODE;
	} else {
		txmode = tc;
		rxmode = SF_DMA_MODE;
	}

	/* configure all channels */
	for (chan = 0; chan < rx_channels_count; chan++) {
		struct stmmac_rx_queue *rx_q = &priv->dma_conf.rx_queue[chan];
		u32 buf_size;

		qmode = priv->plat->rx_queues_cfg[chan].mode_to_use;

		stmmac_dma_rx_mode(priv, priv->ioaddr, rxmode, chan,
				   rxfifosz, qmode);

		if (rx_q->xsk_pool) {
			buf_size = xsk_pool_get_rx_frame_size(rx_q->xsk_pool);
			stmmac_set_dma_bfsize(priv, priv->ioaddr,
					      buf_size,
					      chan);
		} else {
			stmmac_set_dma_bfsize(priv, priv->ioaddr,
					      priv->dma_conf.dma_buf_sz,
					      chan);
		}
	}

	for (chan = 0; chan < tx_channels_count; chan++) {
		qmode = priv->plat->tx_queues_cfg[chan].mode_to_use;

		stmmac_dma_tx_mode(priv, priv->ioaddr, txmode, chan,
				   txfifosz, qmode);
	}
}

static int stmmac_uio_reset(struct stmmac_priv *priv, void __iomem *ioaddr)
{
	struct plat_stmmacenet_data *plat = priv ? priv->plat : NULL;

	if (!priv)
		return -EINVAL;

	if (plat && plat->fix_soc_reset)
		return plat->fix_soc_reset(priv, ioaddr);

	return stmmac_do_callback(priv, dma, reset, ioaddr);
}

/**
 * stmmac_uio_init_dma_engine - DMA init.
 * @priv: driver private structure
 * Description:
 * It inits the DMA invoking the specific MAC/GMAC callback.
 * Some DMA parameters can be passed from the platform;
 * in case of these are not passed a default is kept for the MAC or GMAC.
 */
static int stmmac_uio_init_dma_engine(struct stmmac_priv *priv)
{
	u32 rx_channels_count = priv->plat->rx_queues_to_use;
	u32 tx_channels_count = priv->plat->tx_queues_to_use;
	u32 dma_csr_ch = max(rx_channels_count, tx_channels_count);
	struct stmmac_rx_queue *rx_q;
	struct stmmac_tx_queue *tx_q;
	u32 chan = 0;
	int ret = 0;

	if (!priv->plat->dma_cfg || !priv->plat->dma_cfg->pbl) {
		netdev_err(priv->dev, "Invalid DMA configuration\n");
		return -EINVAL;
	}

	if (priv->extend_desc && (priv->mode == STMMAC_RING_MODE))
		priv->plat->dma_cfg->atds = 1;

	ret = stmmac_uio_reset(priv, priv->ioaddr);
	if (ret) {
		netdev_err(priv->dev, "Failed to reset the dma\n");
		return ret;
	}

	/* DMA Configuration */
	stmmac_dma_init(priv, priv->ioaddr, priv->plat->dma_cfg);

	if (priv->plat->axi)
		stmmac_axi(priv, priv->ioaddr, priv->plat->axi);

	/* DMA CSR Channel configuration */
	for (chan = 0; chan < dma_csr_ch; chan++) {
		stmmac_init_chan(priv, priv->ioaddr, priv->plat->dma_cfg, chan);
		stmmac_disable_dma_irq(priv, priv->ioaddr, chan, 1, 1);
	}

	/* DMA RX Channel Configuration */
	for (chan = 0; chan < rx_channels_count; chan++) {
		rx_q = &priv->dma_conf.rx_queue[chan];

		stmmac_init_rx_chan(priv, priv->ioaddr, priv->plat->dma_cfg,
				    rx_q->dma_rx_phy, chan);

		rx_q->rx_tail_addr = rx_q->dma_rx_phy +
				     (rx_q->buf_alloc_num *
				     sizeof(struct dma_desc));
		stmmac_set_rx_tail_ptr(priv, priv->ioaddr,
				       rx_q->rx_tail_addr, chan);
	}

	/* DMA TX Channel Configuration */
	for (chan = 0; chan < tx_channels_count; chan++) {
		tx_q = &priv->dma_conf.tx_queue[chan];

		stmmac_init_tx_chan(priv, priv->ioaddr, priv->plat->dma_cfg,
				    tx_q->dma_tx_phy, chan);

		tx_q->tx_tail_addr = tx_q->dma_tx_phy;
		stmmac_set_tx_tail_ptr(priv, priv->ioaddr,
				       tx_q->tx_tail_addr, chan);
	}

	return ret;
}

static void stmmac_uio_set_rings_length(struct stmmac_priv *priv)
{
	u32 rx_channels_count = priv->plat->rx_queues_to_use;
	u32 tx_channels_count = priv->plat->tx_queues_to_use;
	u32 chan;

	/* set TX ring length */
	for (chan = 0; chan < tx_channels_count; chan++)
		stmmac_set_tx_ring_len(priv, priv->ioaddr,
				       (priv->dma_conf.dma_tx_size - 1), chan);

	/* set RX ring length */
	for (chan = 0; chan < rx_channels_count; chan++)
		stmmac_set_rx_ring_len(priv, priv->ioaddr,
				       (priv->dma_conf.dma_rx_size - 1), chan);
}

/**
 *  stmmac_uio_set_tx_queue_weight - Set TX queue weight
 *  @priv: driver private structure
 *  Description: It is used for setting TX queues weight
 */
static void stmmac_uio_set_tx_queue_weight(struct stmmac_priv *priv)
{
	u32 tx_queues_count = priv->plat->tx_queues_to_use;
	u32 weight;
	u32 queue;

	for (queue = 0; queue < tx_queues_count; queue++) {
		weight = priv->plat->tx_queues_cfg[queue].weight;
		stmmac_set_mtl_tx_queue_weight(priv, priv->hw, weight, queue);
	}
}

/**
 *  stmmac_uio_configure_cbs - Configure CBS in TX queue
 *  @priv: driver private structure
 *  Description: It is used for configuring CBS in AVB TX queues
 */
static void stmmac_uio_configure_cbs(struct stmmac_priv *priv)
{
	u32 tx_queues_count = priv->plat->tx_queues_to_use;
	u32 mode_to_use;
	u32 queue;

	/* queue 0 is reserved for legacy traffic */
	for (queue = 1; queue < tx_queues_count; queue++) {
		mode_to_use = priv->plat->tx_queues_cfg[queue].mode_to_use;
		if (mode_to_use == MTL_QUEUE_DCB)
			continue;

		stmmac_config_cbs(priv, priv->hw,
				  priv->plat->tx_queues_cfg[queue].send_slope,
				  priv->plat->tx_queues_cfg[queue].idle_slope,
				  priv->plat->tx_queues_cfg[queue].high_credit,
				  priv->plat->tx_queues_cfg[queue].low_credit,
				  queue);
	}
}

/**
 *  stmmac_uio_rx_queue_dma_chan_map - Map RX queue to RX dma channel
 *  @priv: driver private structure
 *  Description: It is used for mapping RX queues to RX dma channels
 */
static void stmmac_uio_rx_queue_dma_chan_map(struct stmmac_priv *priv)
{
	u32 rx_queues_count = priv->plat->rx_queues_to_use;
	u32 queue;
	u32 chan;

	for (queue = 0; queue < rx_queues_count; queue++) {
		chan = priv->plat->rx_queues_cfg[queue].chan;
		stmmac_map_mtl_to_dma(priv, priv->hw, queue, chan);
	}
}

/**
 *  stmmac_mac_uio_config_rx_queues_prio - Configure RX Queue priority
 *  @priv: driver private structure
 *  Description: It is used for configuring the RX Queue Priority
 */
static void stmmac_uio_mac_config_rx_queues_prio(struct stmmac_priv *priv)
{
	u32 rx_queues_count = priv->plat->rx_queues_to_use;
	u32 queue;
	u32 prio;

	for (queue = 0; queue < rx_queues_count; queue++) {
		if (!priv->plat->rx_queues_cfg[queue].use_prio)
			continue;

		prio = priv->plat->rx_queues_cfg[queue].prio;
		stmmac_rx_queue_prio(priv, priv->hw, prio, queue);
	}
}

/**
 *  stmmac_uio_mac_config_tx_queues_prio - Configure TX Queue priority
 *  @priv: driver private structure
 *  Description: It is used for configuring the TX Queue Priority
 */
static void stmmac_uio_mac_config_tx_queues_prio(struct stmmac_priv *priv)
{
	u32 tx_queues_count = priv->plat->tx_queues_to_use;
	u32 queue;
	u32 prio;

	for (queue = 0; queue < tx_queues_count; queue++) {
		if (!priv->plat->tx_queues_cfg[queue].use_prio)
			continue;

		prio = priv->plat->tx_queues_cfg[queue].prio;
		stmmac_tx_queue_prio(priv, priv->hw, prio, queue);
	}
}

/**
 *  stmmac_uio_mac_config_rx_queues_routing - Configure RX Queue Routing
 *  @priv: driver private structure
 *  Description: It is used for configuring the RX queue routing
 */
static void stmmac_uio_mac_config_rx_queues_routing(struct stmmac_priv *priv)
{
	u32 rx_queues_count = priv->plat->rx_queues_to_use;
	u32 queue;
	u8 packet;

	for (queue = 0; queue < rx_queues_count; queue++) {
		/* no specific packet type routing specified for the queue */
		if (priv->plat->rx_queues_cfg[queue].pkt_route == 0x0)
			continue;

		packet = priv->plat->rx_queues_cfg[queue].pkt_route;
		stmmac_rx_queue_routing(priv, priv->hw, packet, queue);
	}
}

static void stmmac_uio_mac_config_rss(struct stmmac_priv *priv)
{
	if (!priv->dma_cap.rssen || !priv->plat->rss_en) {
		priv->rss.enable = false;
		return;
	}

	if (priv->dev->features & NETIF_F_RXHASH)
		priv->rss.enable = true;
	else
		priv->rss.enable = false;

	stmmac_rss_configure(priv, priv->hw, &priv->rss,
			     priv->plat->rx_queues_to_use);
}

/**
 *  stmmac_uio_mtl_configuration - Configure MTL
 *  @priv: driver private structure
 *  Description: It is used for configurring MTL
 */
static void stmmac_uio_mtl_configuration(struct stmmac_priv *priv)
{
	u32 rx_queues_count = priv->plat->rx_queues_to_use;
	u32 tx_queues_count = priv->plat->tx_queues_to_use;

	if (tx_queues_count > 1)
		stmmac_uio_set_tx_queue_weight(priv);

	/* Configure MTL RX algorithms */
	if (rx_queues_count > 1)
		stmmac_prog_mtl_rx_algorithms(priv, priv->hw,
				priv->plat->rx_sched_algorithm);

	/* Configure MTL TX algorithms */
	if (tx_queues_count > 1)
		stmmac_prog_mtl_tx_algorithms(priv, priv->hw,
				priv->plat->tx_sched_algorithm);

	/* Configure CBS in AVB TX queues */
	if (tx_queues_count > 1)
		stmmac_uio_configure_cbs(priv);

	/* Map RX MTL to DMA channels */
	stmmac_uio_rx_queue_dma_chan_map(priv);

	/* Enable MAC RX Queues */
	stmmac_uio_mac_enable_rx_queues(priv);

	/* Set RX priorities */
	if (rx_queues_count > 1)
		stmmac_uio_mac_config_rx_queues_prio(priv);

	/* Set TX priorities */
	if (tx_queues_count > 1)
		stmmac_uio_mac_config_tx_queues_prio(priv);

	/* Set RX routing */
	if (rx_queues_count > 1)
		stmmac_uio_mac_config_rx_queues_routing(priv);

	/* Receive Side Scaling */
	if (rx_queues_count > 1)
		stmmac_uio_mac_config_rss(priv);
}

static void stmmac_uio_safety_feat_configuration(struct stmmac_priv *priv)
{
	if (priv->dma_cap.asp) {
		netdev_info(priv->dev, "Enabling Safety Features\n");
		stmmac_safety_feat_config(priv, priv->ioaddr, priv->dma_cap.asp,
					  priv->plat->safety_feat_cfg);
	} else {
		netdev_info(priv->dev, "No Safety Features support found\n");
	}
}

/**
 * stmmac_uio_hw_setup - setup mac in a usable state.
 *  @dev : pointer to the device structure.
 *  Description:
 *  this is the main function to setup the HW in a usable state because the
 *  dma engine is reset, the core registers are configured (e.g. AXI,
 *  Checksum features, timers). The DMA is ready to start receiving and
 *  transmitting.
 *  Return value:
 *  0 on success and an appropriate (-)ve integer as defined in errno.h
 *  file on failure.
 */
static int stmmac_uio_hw_setup(struct net_device *dev)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	int ret;

	/* Make sure RX clock is enabled */
	if (priv->hw->phylink_pcs)
		phylink_pcs_pre_init(priv->phylink, priv->hw->phylink_pcs);

	/* Note that clk_rx_i must be running for reset to complete. This
	 * clock may also be required when setting the MAC address.
	 *
	 * Block the receive clock stop for LPI mode at the PHY in case
	 * the link is established with EEE mode active.
	 */
	phylink_rx_clk_stop_block(priv->phylink);

	/* DMA initialization and SW reset */
	ret = stmmac_uio_init_dma_engine(priv);
	if (ret < 0) {
		phylink_rx_clk_stop_unblock(priv->phylink);
		netdev_err(priv->dev, "%s: DMA engine initialization failed\n",
			   __func__);
		return ret;
	}

	/* Copy the MAC addr into the HW  */
	stmmac_set_umac_addr(priv, priv->hw, dev->dev_addr, 0);
	phylink_rx_clk_stop_unblock(priv->phylink);

	/* PS and related bits will be programmed according to the speed */
	if (priv->hw->pcs) {
		int speed = priv->plat->mac_port_sel_speed;

		if ((speed == SPEED_10) || (speed == SPEED_100) ||
		    (speed == SPEED_1000)) {
			priv->hw->ps = speed;
		} else {
			dev_warn(priv->device, "invalid port speed\n");
			priv->hw->ps = 0;
		}
	}

	/* Initialize the MAC Core */
	stmmac_core_init(priv, priv->hw, dev);

	/* Initialize MTL*/
	stmmac_uio_mtl_configuration(priv);

	/* Initialize Safety Features */
	stmmac_uio_safety_feat_configuration(priv);

	ret = stmmac_rx_ipc(priv, priv->hw);
	if (!ret) {
		netdev_warn(priv->dev, "RX IPC Checksum Offload disabled\n");
		priv->plat->rx_coe = STMMAC_RX_COE_NONE;
		priv->hw->rx_csum = 0;
	}

	/* Enable the MAC Rx/Tx */
	stmmac_mac_set(priv, priv->ioaddr, true);

	/* Set the HW DMA mode and the COE */
	stmmac_uio_dma_operation_mode(priv);

	if (priv->hw->pcs)
		stmmac_pcs_ctrl_ane(priv, 1, priv->hw->ps, 0);

	/* set TX and RX rings length */
	stmmac_uio_set_rings_length(priv);

	return 0;
}

/**
 *  stmmac_uio_setup_dma_desc - Generate a dma_conf and allocate DMA queue
 *  @priv: driver private structure
 *  @mtu: MTU to setup the dma queue and buf with
 *  Description: Allocate and generate a dma_conf based on the provided MTU.
 *  Allocate the Tx/Rx DMA queue and init them.
 *  Return value:
 *  the dma_conf allocated struct on success and an appropriate ERR_PTR on failure.
 */
static struct stmmac_dma_conf *
stmmac_uio_setup_dma_desc(struct stmmac_priv *priv, unsigned int mtu)
{
	struct stmmac_dma_conf *dma_conf;
	int chan, bfsize, ret;

	dma_conf = kzalloc(sizeof(*dma_conf), GFP_KERNEL);
	if (!dma_conf) {
		netdev_err(priv->dev, "%s: DMA conf allocation failed\n",
			   __func__);
		return ERR_PTR(-ENOMEM);
	}

	bfsize = stmmac_set_16kib_bfsize(priv, mtu);
	if (bfsize < 0)
		bfsize = 0;

	if (bfsize < BUF_SIZE_16KiB)
		bfsize = stmmac_uio_set_bfsize(mtu, 0);

	dma_conf->dma_buf_sz = bfsize;
	dma_conf->dma_tx_size = DMA_MAX_TX_SIZE;
	dma_conf->dma_rx_size = DMA_MAX_RX_SIZE;

	/* Earlier check for TBS */
	for (chan = 0; chan < priv->plat->tx_queues_to_use; chan++) {
		struct stmmac_tx_queue *tx_q = &dma_conf->tx_queue[chan];
		int tbs_en = priv->plat->tx_queues_cfg[chan].tbs_en;

		/* Setup per-TXQ tbs flag before TX descriptor alloc */
		tx_q->tbs |= tbs_en ? STMMAC_TBS_AVAIL : 0;
	}

	ret = uio_alloc_dma_desc_resources(priv, dma_conf);
	if (ret < 0) {
		netdev_err(priv->dev, "%s: DMA descriptors allocation failed\n",
			   __func__);
		goto alloc_error;
	}

	return dma_conf;

alloc_error:
	kfree(dma_conf);
	return ERR_PTR(ret);
}

/**
 *  __uio_stmmac_open - open entry point of the driver
 *  @dev : pointer to the device structure.
 *  @dma_conf :  structure to take the dma data
 *  Description:
 *  This function is the open entry point of the driver.
 *  Return value:
 *  0 on success and an appropriate (-)ve integer as defined in errno.h
 *  file on failure.
 */
static int __stmmac_uio_open(struct net_device *dev,
			 struct stmmac_dma_conf *dma_conf)
{
	struct stmmac_priv *priv = netdev_priv(dev);
	int ret;

	memcpy(&priv->dma_conf, dma_conf, sizeof(*dma_conf));

	ret = stmmac_uio_hw_setup(dev);
	if (ret < 0) {
		netdev_err(priv->dev, "%s: Hw setup failed\n", __func__);
		goto init_error;
	}

	phylink_start(priv->phylink);
	/* We may have called phylink_speed_down before */
	phylink_speed_up(priv->phylink);

	return 0;

init_error:
	return ret;
}

/**
 *  stmmac_uio_open - open entry point of the driver
 *  @dev : pointer to the device structure.
 *  Description:
 *  This function is the open entry point of the driver.
 *  Return value:
 *  0 on success and an appropriate (-)ve integer as defined in errno.h
 *  file on failure.
 */
static int stmmac_uio_open(struct spacemit_gmac_uio_priv *uio_priv)
{
	struct net_device *dev = uio_priv->ndev;
	struct stmmac_priv *priv = netdev_priv(dev);
	struct stmmac_dma_conf *dma_conf;
	int ret;

	memcpy(&uio_priv->orig_dma_conf, &priv->dma_conf, sizeof(priv->dma_conf));
	dma_conf = stmmac_uio_setup_dma_desc(priv, dev->mtu);
	if (IS_ERR(dma_conf))
		return PTR_ERR(dma_conf);

	ret = pm_runtime_resume_and_get(priv->device);
	if (ret < 0)
		goto err_dma_resources;

	ret = stmmac_uio_init_phy(dev);
	if (ret)
		goto err_runtime_pm;

	ret = __stmmac_uio_open(dev, dma_conf);
	if (ret)
		goto err_disconnect_phy;

	kfree(dma_conf);

	return ret;

err_disconnect_phy:
	memcpy(&priv->dma_conf, &uio_priv->orig_dma_conf, sizeof(priv->dma_conf));
	phylink_disconnect_phy(priv->phylink);
err_runtime_pm:
	pm_runtime_put(priv->device);
err_dma_resources:
	uio_free_dma_desc_resources(priv, dma_conf);
	kfree(dma_conf);
	return ret;
}

/**
 *  stmmac_uio_release - close entry point of the driver
 *  @dev : device pointer.
 *  Description:
 *  This is the stop entry point of the driver.
 */
static int stmmac_uio_release(struct spacemit_gmac_uio_priv *uio_priv)
{
	struct net_device *dev = uio_priv->ndev;
	struct stmmac_priv *priv = netdev_priv(dev);

	/* Stop and disconnect the PHY */
	phylink_stop(priv->phylink);
	phylink_disconnect_phy(priv->phylink);

	/* Release and free the Rx/Tx resources */
	uio_free_dma_desc_resources(priv, &priv->dma_conf);
	memcpy(&priv->dma_conf, &uio_priv->orig_dma_conf, sizeof(priv->dma_conf));

	pm_runtime_put(priv->device);

	return 0;
}

/**
 * spacemit_uio_takeover_hardware() - Take over control of the MAC hardware
 * @uio_priv: Pointer to the private UIO driver data structure
 *
 * This function shuts down the kernel's native network operations on the
 * device and switches the hardware to the dedicated DMA configuration
 * reserved for the userspace UIO/DPDK application.
 *
 * Context: Must be called with RTNL lock omitted inside, as it internally
 *          acquires and releases rtnl_lock() for dev_close().
 * Return: 0 on success, negative error code on failure.
 */
static int spacemit_uio_takeover_hardware(struct spacemit_gmac_uio_priv *uio_priv)
{
	int err;

	rtnl_lock();
	dev_close(uio_priv->ndev);
	err = stmmac_uio_open(uio_priv);
	rtnl_unlock();

	return err;
}

/**
 * spacemit_uio_restore_hardware() - Restore hardware control back to the kernel
 * @uio_priv: Pointer to the private UIO driver data structure
 *
 * This function performs the reverse operations of hardware takeover. It frees
 * the UIO-specific DMA descriptor rings, re-opens the net_device to reactivate
 * the native kernel network stack.
 *
 * Context: This function can be called either immediately during an error path
 *          in probe(), synchronously during remove() if no active userspace
 *          mappings exist, or deferred to uio_release() when the last userspace
 *          application closes the file descriptor.
 */
static void spacemit_uio_restore_hardware(struct spacemit_gmac_uio_priv *uio_priv)
{
	if (!uio_priv->ndev)
		return;

	rtnl_lock();
	stmmac_uio_release(uio_priv);
	dev_open(uio_priv->ndev, NULL);
	rtnl_unlock();
}

/**
 * spacemit_gmac_uio_probe() platform driver probe routine
 * - register uio devices filled with memory maps retrieved
 * from device tree
 */
static int spacemit_gmac_uio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *mac_node;
	struct spacemit_gmac_uio_priv *uio_priv;
	struct net_device *netdev;
	struct stmmac_priv *priv;
	struct uio_info *uio;
	struct resource res;
	int err = 0;

	uio_priv = kzalloc(sizeof(struct spacemit_gmac_uio_priv), GFP_KERNEL);
	if (!uio_priv)
		return -ENOMEM;

	uio = &uio_priv->uio;
	uio_priv->dev = dev;

	atomic_set(&uio_priv->refcnt, 1);
	uio_priv->flags = 0;

	mac_node = of_parse_phandle(np, "spacemit,ethernet", 0);
	if (!mac_node) {
		dev_err(dev, "Failed to get spacemit,ethernet node\n");
		kfree(uio_priv);
		return -ENODEV;
	}

	err = of_address_to_resource(mac_node, 0, &res);
	if (err) {
		dev_err(dev, "Failed to parse mem resource from eth0 netdev node: %d\n", err);
		of_node_put(mac_node);
		kfree(uio_priv);
		return err;
	}

	if (of_device_is_available(mac_node)) {
		netdev = of_find_net_device_by_node(mac_node);
		err = netdev ? 0: -ENODEV;
	} else {
		err = -EINVAL;
	}

	of_node_put(mac_node);

	if (err) {
		dev_err(dev, "Failed to get net_device from spacemit,ethernet node: %d\n", err);
		kfree(uio_priv);
		return err;
	}

	uio_priv->ndev = netdev;

	/* Smoothly take over control of the MAC hardware */
	err = spacemit_uio_takeover_hardware(uio_priv);
	if (err)
		goto err_put_netdev;

	priv = netdev_priv(netdev);
	snprintf(uio_priv->name, sizeof(uio_priv->name), "%s",
		 STMMAC_UIO_DEVICE_NAME);
	uio->name = uio_priv->name;
	uio->version = DRIVER_VERSION;

	uio->mem[0].name = "eth_regs";
	uio->mem[0].addr = res.start & PAGE_MASK;
	uio->mem[0].size = PAGE_ALIGN(resource_size(&res));
	uio->mem[0].memtype = UIO_MEM_PHYS;

	uio->mem[1].name = "eth_rx_bd";
	uio->mem[1].addr = priv->dma_conf.rx_queue[0].dma_rx_phy;
	uio->mem[1].size = priv->dma_conf.dma_rx_size * sizeof(struct dma_desc);
	uio->mem[1].memtype = UIO_MEM_PHYS;

	uio->mem[2].name = "eth_tx_bd";
	uio->mem[2].addr = priv->dma_conf.tx_queue[0].dma_tx_phy;
	uio->mem[2].size = priv->dma_conf.dma_tx_size * sizeof(struct dma_desc);
	uio->mem[2].memtype = UIO_MEM_PHYS;

	dev_info(dev, "UIO memory maps: %s at 0x%016llx, %s at 0x%016llx, %s at 0x%016llx\n",
		 uio->mem[0].name, uio->mem[0].addr,
		 uio->mem[1].name, uio->mem[1].addr,
		 uio->mem[2].name, uio->mem[2].addr);

	uio->open = spacemit_gmac_uio_open;
	uio->release = spacemit_gmac_uio_release;
	/* mmap function. */
	uio->mmap = spacemit_gmac_uio_mmap;
	uio->priv = uio_priv;

	err = uio_register_device(dev, uio);
	if (err) {
		dev_err(dev, "Failed to register uio device: %d\n", err);
		goto err_restore_hw;
	}

	uio_priv->map_num = 3;

	platform_set_drvdata(pdev, uio_priv);

	return 0;

err_restore_hw:
	spacemit_uio_restore_hardware(uio_priv);
err_put_netdev:
	dev_put(netdev);
	kfree(uio_priv);
	return err;
}

/**
 * spacemit_gmac_uio_remove() - Spacemit gmac uio platform driver release
 * routine - unregister uio devices
 */
static void spacemit_gmac_uio_remove(struct platform_device *pdev)
{
	struct spacemit_gmac_uio_priv *uio_priv = platform_get_drvdata(pdev);
	struct net_device *netdev;

	if (!uio_priv)
		return ;

	platform_set_drvdata(pdev, NULL);
	netdev = uio_priv->ndev;

	set_bit(UIO_FLAGS_REMOVED, &uio_priv->flags);

	uio_unregister_device(&uio_priv->uio);

	if (atomic_dec_and_test(&uio_priv->refcnt)) {
		pr_info(DRIVER_NAME ": Final closure from remove(): Restoring hardware back to native netdev.\n");
		spacemit_uio_restore_hardware(uio_priv);
		if (netdev)
			dev_put(netdev);
		kfree(uio_priv);
		return;
	}

	pr_info(DRIVER_NAME ": Userspace active. Hardware restoration deferred.\n");
}

static const struct of_device_id spacemit_gmac_uio_of_match[] = {
	{ .compatible	= "k3,stmmac-uio", },
	{ }
};

static struct platform_driver spacemit_gmac_uio_driver = {
	.driver = {
		.owner		= THIS_MODULE,
		.name		= DRIVER_NAME,
		.of_match_table	= spacemit_gmac_uio_of_match,
	},
	.probe	= spacemit_gmac_uio_probe,
	.remove	= spacemit_gmac_uio_remove,
};

module_platform_driver(spacemit_gmac_uio_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Spacemit gmac uio driver");
MODULE_AUTHOR("Spacemit");