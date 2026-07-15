// SPDX-License-Identifier: GPL-2.0

#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/kthread.h>
#include <linux/clk-provider.h>
#include <linux/mailbox_client.h>
#include <linux/completion.h>
#include <linux/freezer.h>
#include <linux/firmware.h>
#include <linux/elf.h>
#include <uapi/linux/sched/types.h>
#include <uapi/linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/rpmsg.h>
#include <linux/pm_qos.h>
#include <linux/delay.h>
#include <linux/syscore_ops.h>
#include <linux/fs.h>
#include <linux/pm_domain.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-direction.h>
#include <linux/suspend.h>
#include <linux/platform_device.h>
#ifdef CONFIG_HIBERNATION
#include <linux/vmalloc.h>
#include <linux/memremap.h>
#include <linux/libnvdimm.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/suspend.h>
#endif
#include "remoteproc_internal.h"
#include "remoteproc_elf_helpers.h"

/* ========================================================================
 * Section 1: Data types and constants
 * ======================================================================== */

#define MAX_MEM_BASE	2
#define MAX_MBOX	2

#define K3_MBOX_VQ0_ID	0
#define K3_MBOX_VQ1_ID	1

/* Maximum sane size for a resource table */
#define RSC_TABLE_MAX_SIZE	SZ_64K

struct spacemit_mbox {
	const char *name;
	struct mbox_chan *chan;
	struct mbox_client client;
	struct task_struct *mb_thread;
	struct completion mb_comp;
	int vq_id;
};

struct spacemit_rproc {
	struct device *dev;
	struct spacemit_mbox mb[MAX_MBOX];
	unsigned int size;
	void __iomem *rsc_table_va;	 /* ioremap'd I/O window, for write-back */
	struct resource_table *rsc_table_ptr; /* kmalloc'd copy returned to core */
};


#ifdef CONFIG_HIBERNATION
#define RCPU_CORE0_BOOT_ENTRY_LO	0xc088007c
#define RCPU_CORE0_BOOT_ENTRY_HI	0xc0880080
#define RCPU_BOOT_ENTRY_SIZE		8	/* LO + HI, 2 × u32 */

#define SPACEMIT_HIBERRESTORE_MAGIC	0x52455354U	/* "REST" — set by PM_RESTORE_PREPARE */
#define SPACEMIT_HIBERSNAP_DONE_MAGIC	0x444f4e45U	/* "DONE" — set after snap restored, before cpu_suspend */

/*
 * Sub-region offsets and sizes within the merged "hibernation_nomap" DT node
 * (base = 0x100700000, total = 0x85400).
 */
#define HIBER_AP_MISC_OFFSET		0x000400UL	/* hibernation_ap_misc */
#define HIBER_AP_MISC_SIZE		0x001000UL
#define HIBER_SNAP_MISC_OFFSET		0x001400UL	/* hibernation_snap_misc */
#define HIBER_SNAP_MISC_SIZE		0x084000UL

static struct {
	/* hibernation_snap_rcpu: rcpu0(5M) + rcpu1(5M) + opensbi(2M), backed up as one block */
	void    *snap_rcop_va;
	size_t   snap_rcop_size;
	void    *snap_backup;	/* vmalloc'd backup of snap_rcop_va + snap_srrpi_va; in hibernation image */
	void    *snap_srrpi_va;	/* hibernation_snap_misc: SRAM + RPMI snapshots */
	size_t   snap_srrpi_size;
	void __iomem *rcpu0_boot_entry_va;	/* maps CORE0 LO/HI regs */
	unsigned long rcpu0_boot_entry;
	void __iomem *hiber_apuse_va;	/* no-map region, survives hibernation image restore */
} spacemit_rproc_ctx;

static bool spacemit_rproc_hibernating;
#endif /* CONFIG_HIBERNATION */

/* ========================================================================
 * Section 2: Remote processor ops
 * ======================================================================== */

static int spacemit_rproc_mem_alloc(struct rproc *rproc, struct rproc_mem_entry *mem)
{
	void __iomem *va = NULL;

	dev_dbg(&rproc->dev, "map memory: %pa+%zx\n", &mem->dma, mem->len);
	va = ioremap(mem->dma, mem->len);
	if (!va) {
		dev_err(&rproc->dev, "Unable to map memory region: %pa+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	/* Update memory entry va */
	mem->va = va;

	return 0;
}

static int spacemit_rproc_mem_release(struct rproc *rproc, struct rproc_mem_entry *mem)
{
	dev_dbg(&rproc->dev, "unmap memory: %pa\n", &mem->dma);

	iounmap(mem->va);

	return 0;
}

static int spacemit_rproc_prepare(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct of_phandle_iterator it;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	int index = 0;

	/* Register associated reserved memory regions */
	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			dev_err(&rproc->dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}

		if (rmem->base > U64_MAX) {
			dev_err(&rproc->dev, "the rmem base is overflow\n");
			return -EINVAL;
		}

		mem = rproc_mem_entry_init(dev, NULL,
					   rmem->base,
					   rmem->size, rmem->base,
					   spacemit_rproc_mem_alloc,
					   spacemit_rproc_mem_release,
					   it.node->name);
		if (!mem)
			return -ENOMEM;

		rproc_add_carveout(rproc, mem);
		index++;
	}

	return 0;
}

static int spacemit_rproc_start(struct rproc *rproc)
{
	/* Do nothing: has been latched from spl */
	return 0;
}

static int spacemit_rproc_stop(struct rproc *rproc)
{
	/* TODO */

	return 0;
}

static void spacemit_rproc_kick(struct rproc *rproc, int vqid)
{
	struct spacemit_rproc *ddata = rproc->priv;
	unsigned int i;
	int err;

	if (WARN_ON(vqid >= MAX_MBOX))
		return;

	for (i = 0; i < MAX_MBOX; i++) {
		if (vqid != ddata->mb[i].vq_id)
			continue;
		if (!ddata->mb[i].chan)
			return;
		err = mbox_send_message(ddata->mb[i].chan, "kick");
		if (err < 0)
			dev_err(&rproc->dev, "%s: failed (%s, err:%d)\n",
				__func__, ddata->mb[i].name, err);
		return;
	}
}

static int spacemit_rproc_attach(struct rproc *rproc)
{
	return 0;
}

static int spacemit_rproc_detach(struct rproc *rproc)
{
	struct spacemit_rproc *priv = rproc->priv;

	/*
	 * The core's rproc_reset_rsc_table_on_detach() writes the clean
	 * resource table back via plain memcpy(table_ptr, clean_table, sz),
	 * which updates only our kmalloc'd heap copy (rsc_table_ptr).
	 * The remote processor's actual reserved-memory region is unaffected.
	 * Sync the updated heap copy back to the I/O region now so the remote
	 * processor sees a clean table on the next attach.
	 */
	if (priv->rsc_table_va && priv->rsc_table_ptr)
		memcpy_toio(priv->rsc_table_va, priv->rsc_table_ptr,
			    rproc->table_sz);

	return 0;
}

/*
 * spacemit_get_loaded_rsc_table - return a snapshot of the resource table
 *                                  installed by the remote processor.
 *
 * The remoteproc core passes the returned pointer to kmemdup() (plain
 * memcpy) and later uses it as rproc->table_ptr for ordinary load/store
 * access.  On RISC-V, device-memory mappings created by ioremap() cannot
 * be accessed with plain load instructions — doing so triggers a load
 * access fault in __memcpy.  We therefore copy the table from the I/O
 * region into a kmalloc buffer using memcpy_fromio() and return that.
 *
 * The rcpu*_rsc_table regions carry "no-map" in DT, so ioremap() is the
 * correct accessor; memremap(MEMREMAP_WB) would not work on them.
 *
 * Write-back on detach: the core's rproc_reset_rsc_table_on_detach()
 * writes the clean table back via plain memcpy(table_ptr, ...), which
 * updates only our heap copy — the remote processor's reserved-memory
 * region is unaffected.  spacemit_rproc_detach() handles the actual
 * write-back to the I/O region using memcpy_toio().
 *
 * Ownership: the kmalloc buffer is stored in priv->rsc_table_ptr and
 * freed in spacemit_rproc_remove().  On repeated attach() calls the old
 * buffer and ioremap mapping are released before new ones are created.
 * The remoteproc core never frees the pointer returned here.
 */
static struct resource_table *spacemit_get_loaded_rsc_table(
				struct rproc *rproc, size_t *size)
{
	struct spacemit_rproc *priv = rproc->priv;
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct of_phandle_iterator it;
	struct reserved_mem *rmem;
	void __iomem *io_va;
	struct resource_table *table;

	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		if (strcmp(it.node->name, "rcpu0_rsc_table") &&
		    strcmp(it.node->name, "rcpu1_rsc_table"))
			continue;

		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			dev_err(&rproc->dev, "unable to acquire memory-region %s\n",
				it.node->name);
			of_node_put(it.node);
			return NULL;
		}

		if (rmem->size > RSC_TABLE_MAX_SIZE) {
			dev_err(&rproc->dev,
				"memory-region %s too large (%zu > %u), suspicious DT\n",
				it.node->name, (size_t)rmem->size,
				RSC_TABLE_MAX_SIZE);
			of_node_put(it.node);
			return ERR_PTR(-EINVAL);
		}

		/* Release any mapping left from a previous attach() */
		if (priv->rsc_table_va) {
			iounmap(priv->rsc_table_va);
			priv->rsc_table_va = NULL;
		}
		kfree(priv->rsc_table_ptr);
		priv->rsc_table_ptr = NULL;

		io_va = ioremap(rmem->base, rmem->size);
		if (!io_va) {
			dev_err(&rproc->dev, "ioremap failed for %s\n",
				it.node->name);
			of_node_put(it.node);
			return ERR_PTR(-ENOMEM);
		}

		table = kmalloc(rmem->size, GFP_KERNEL);
		if (!table) {
			iounmap(io_va);
			of_node_put(it.node);
			return ERR_PTR(-ENOMEM);
		}

		/*
		 * memcpy_fromio() is required here — ioremap() VA cannot be
		 * read with plain load instructions on RISC-V.
		 */
		memcpy_fromio(table, io_va, rmem->size);

		priv->rsc_table_va  = io_va;
		priv->rsc_table_ptr = table;
		*size = rmem->size;

		of_node_put(it.node);
		return table;
	}

	return NULL;
}

static struct rproc_ops spacemit_rproc_ops = {
	.prepare	= spacemit_rproc_prepare,
	.start		= spacemit_rproc_start,
	.stop		= spacemit_rproc_stop,
	.attach		= spacemit_rproc_attach,
	.detach		= spacemit_rproc_detach,
	.kick		= spacemit_rproc_kick,
	.get_loaded_rsc_table	= spacemit_get_loaded_rsc_table,
};

static int __process_theread(void *arg)
{
	int ret;
	struct mbox_client *cl = arg;
	struct rproc *rproc = dev_get_drvdata(cl->dev);
	struct spacemit_mbox *mb = container_of(cl, struct spacemit_mbox, client);
	struct sched_param param = {.sched_priority = 0 };

	ret = sched_setscheduler(current, SCHED_FIFO, &param);
	set_freezable();

	do {
		try_to_freeze();
		wait_for_completion_timeout(&mb->mb_comp, 10);
		if (rproc_vq_interrupt(rproc, mb->vq_id) == IRQ_NONE)
			dev_dbg(&rproc->dev, "no message found in vq%d\n", mb->vq_id);
	} while (!kthread_should_stop());

	return 0;
}

static void k3_rproc_mb_callback(struct mbox_client *cl, void *data)
{
	struct spacemit_mbox *mb = container_of(cl, struct spacemit_mbox, client);

	complete(&mb->mb_comp);
}

/* ========================================================================
 * Section 3: Hibernation subsystem
 * ======================================================================== */

#ifdef CONFIG_HIBERNATION
extern unsigned long spacemit_suspend_ctx_va;
asmlinkage int spacemit_cpu_resume_enter(unsigned long hartid, unsigned long context);

static int rproc_system_suspend(unsigned long sleep_type,
                              unsigned long resume_addr,
                              unsigned long opaque)
{
	struct sbiret ret;

	/* flush the local cache */
	sbi_flush_local_dcache_all();

	/*
	 * Save the context VA in a .data global before the SBI ecall so
	 * spacemit_cpu_resume_enter() can load it directly without relying
	 * on a1 from OpenSBI (which replays a stale snapshot address).
	 */
	spacemit_suspend_ctx_va = opaque;
	sbi_flush_local_dcache_all();

	ret = sbi_ecall(SBI_EXT_SUSP, SBI_EXT_SUSP_SYSTEM_SUSPEND,
			sleep_type,
			__pa_symbol(spacemit_cpu_resume_enter),
			opaque, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);

	return ret.value;
}

static int spacemit_rproc_pm_notifier(struct notifier_block *nb,
				      unsigned long event, void *data)
{
	switch (event) {
	case PM_HIBERNATION_PREPARE:
		spacemit_rproc_hibernating = true;
		break;
	case PM_RESTORE_PREPARE:
		/* write to no-map region so the flag survives memory restore.
		 * If DONE is already present, opensbi already ran on the prior boot
		 * and restored the small cores — don't overwrite with REST. */
		if (spacemit_rproc_ctx.hiber_apuse_va &&
		    readl(spacemit_rproc_ctx.hiber_apuse_va) != SPACEMIT_HIBERSNAP_DONE_MAGIC)
			writel(SPACEMIT_HIBERRESTORE_MAGIC,
			       spacemit_rproc_ctx.hiber_apuse_va);
		break;
	case PM_POST_HIBERNATION:
		spacemit_rproc_hibernating = false;
		break;
	case PM_POST_RESTORE:
		if (spacemit_rproc_ctx.hiber_apuse_va)
			writel(0, spacemit_rproc_ctx.hiber_apuse_va);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block spacemit_rproc_pm_nb = {
	.notifier_call = spacemit_rproc_pm_notifier,
};

static int spacemit_rproc_syscore_suspend(void)
{
	int ret;

	if (!spacemit_rproc_hibernating)
		return 0;

	/* suspend to DISK so the rcpu writes its snapshots into the no-map regions */
	ret = cpu_suspend(SBI_SUSP_SLEEP_TYPE_SUSPEND_TO_DISK, rproc_system_suspend);
	if (ret) {
		pr_err("k3-rproc: rcpu snapshot suspend failed (%d), aborting hibernation\n", ret);
		return ret;
	}

	/* invalidate CPU dcache over both no-map regions (memremap addresses — use
	 * arch_invalidate_pmem, not flush_dcache_page which only works on linear-map VAs)
	 * so the hibernation framework reads rcpu-written data when building the image */
	arch_invalidate_pmem(spacemit_rproc_ctx.snap_rcop_va, spacemit_rproc_ctx.snap_rcop_size);
	arch_invalidate_pmem(spacemit_rproc_ctx.snap_srrpi_va, spacemit_rproc_ctx.snap_srrpi_size);

	/* snap_rcop_va covers rcpu0(5M) + rcpu1(5M) + opensbi(2M) contiguously — back up
	 * the whole block at once.  snap_srrpi_va (SRAM + RPMI) follows immediately. */
	memcpy(spacemit_rproc_ctx.snap_backup,
	       spacemit_rproc_ctx.snap_rcop_va,
	       spacemit_rproc_ctx.snap_rcop_size);
	memcpy(spacemit_rproc_ctx.snap_backup + spacemit_rproc_ctx.snap_rcop_size,
	       spacemit_rproc_ctx.snap_srrpi_va,
	       spacemit_rproc_ctx.snap_srrpi_size);

	arch_wb_cache_pmem(spacemit_rproc_ctx.snap_backup,
			   spacemit_rproc_ctx.snap_rcop_size +
			   spacemit_rproc_ctx.snap_srrpi_size);

	return 0;
}

static void spacemit_rproc_syscore_resume(void)
{
	u32 magic;
	int ret;

	if (!spacemit_rproc_ctx.hiber_apuse_va)
		return;

	magic = readl(spacemit_rproc_ctx.hiber_apuse_va);

	if (magic == SPACEMIT_HIBERSNAP_DONE_MAGIC) {
		/* opensbi already restored the small cores on the previous warm
		 * boot; just clear the flag and let hibernation resume proceed */
		writel(0, spacemit_rproc_ctx.hiber_apuse_va);
		return;
	}

	if (magic != SPACEMIT_HIBERRESTORE_MAGIC)
		return;

	/* clear immediately so a crash-loop cannot re-trigger */
	writel(0, spacemit_rproc_ctx.hiber_apuse_va);

	/* zero rcpu0 boot entry so opensbi knows to restore from snapshot */
	writel(0, spacemit_rproc_ctx.rcpu0_boot_entry_va);
	writel(0, spacemit_rproc_ctx.rcpu0_boot_entry_va + 4);

	/* restore snap_rcop_va (rcpu0 + rcpu1 + opensbi) and snap_srrpi_va (SRAM + RPMI)
	 * from snap_backup into the no-map regions before handing off to opensbi */
	memcpy(spacemit_rproc_ctx.snap_rcop_va,
	       spacemit_rproc_ctx.snap_backup,
	       spacemit_rproc_ctx.snap_rcop_size);
	arch_wb_cache_pmem(spacemit_rproc_ctx.snap_rcop_va, spacemit_rproc_ctx.snap_rcop_size);

	memcpy(spacemit_rproc_ctx.snap_srrpi_va,
	       spacemit_rproc_ctx.snap_backup + spacemit_rproc_ctx.snap_rcop_size,
	       spacemit_rproc_ctx.snap_srrpi_size);
	arch_wb_cache_pmem(spacemit_rproc_ctx.snap_srrpi_va, spacemit_rproc_ctx.snap_srrpi_size);

	ret = cpu_suspend(SBI_SUSP_SLEEP_TYPE_SUSPEND_TO_DISK, rproc_system_suspend);
	if (ret)
		pr_err("k3-rproc: rcpu snapshot restore failed (%d), rcpu state undefined\n", ret);
}

static struct syscore_ops spacemit_rcpu0_syscore_ops = {
	.suspend = spacemit_rproc_syscore_suspend,
	.resume  = spacemit_rproc_syscore_resume,
};

static int spacemit_rproc_snap_rcop_init(struct device *dev)
{
	struct device_node *snap_np;
	struct reserved_mem *rmem;

	/* hibernation_snap[1]: hibernation_snap_rcpu (rcpu0+rcpu1+opensbi snapshots) */
	snap_np = of_parse_phandle(dev->of_node, "hibernation_snap", 1);
	if (!snap_np) {
		dev_err(dev, "hibernation_snap[1] node not found\n");
		return -ENODEV;
	}

	rmem = of_reserved_mem_lookup(snap_np);
	of_node_put(snap_np);
	if (!rmem) {
		dev_err(dev, "hibernation_snap[1] reserved_mem lookup failed\n");
		return -ENODEV;
	}

	spacemit_rproc_ctx.snap_rcop_va = memremap(rmem->base, rmem->size, MEMREMAP_WB);
	if (!spacemit_rproc_ctx.snap_rcop_va) {
		dev_err(dev, "memremap hibernation_snap failed\n");
		return -ENOMEM;
	}
	/* snap_rcop_size covers the full region: rcpu0(5M) + rcpu1(5M) + opensbi(2M) */
	spacemit_rproc_ctx.snap_rcop_size = rmem->size;
	return 0;
}

static int spacemit_rproc_hiber_apuse_init(struct device *dev)
{
	struct device_node *np;
	struct reserved_mem *rmem;

	/* hibernation_snap[0]: hibernation_nomap (misc + AP state + SRAM/RPMI snapshots) */
	np = of_parse_phandle(dev->of_node, "hibernation_snap", 0);
	if (!np) {
		dev_err(dev, "hibernation_snap[0] node not found\n");
		return -ENODEV;
	}

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem) {
		dev_err(dev, "hibernation_snap[0] reserved_mem lookup failed\n");
		return -ENODEV;
	}

	if (rmem->size < HIBER_AP_MISC_OFFSET + HIBER_AP_MISC_SIZE) {
		dev_err(dev, "hibernation_snap[0] too small: %pa < 0x%lx\n",
			&rmem->size, HIBER_AP_MISC_OFFSET + HIBER_AP_MISC_SIZE);
		return -EINVAL;
	}

	spacemit_rproc_ctx.hiber_apuse_va =
		ioremap(rmem->base + HIBER_AP_MISC_OFFSET, HIBER_AP_MISC_SIZE);
	if (!spacemit_rproc_ctx.hiber_apuse_va) {
		dev_err(dev, "ioremap hibernation_store_misc failed\n");
		return -ENOMEM;
	}

	/* preserve DONE magic across the warm-boot so syscore_resume can detect
	 * that opensbi already restored the small cores; clear everything else */
	if (readl(spacemit_rproc_ctx.hiber_apuse_va) != SPACEMIT_HIBERSNAP_DONE_MAGIC)
		writel(0, spacemit_rproc_ctx.hiber_apuse_va);
	return 0;
}

static int spacemit_rproc_snap_srrpi_init(struct device *dev)
{
	struct device_node *np;
	struct reserved_mem *rmem;

	/* hibernation_snap[0]: hibernation_nomap (SRAM + RPMI snapshots at +HIBER_SNAP_MISC_OFFSET) */
	np = of_parse_phandle(dev->of_node, "hibernation_snap", 0);
	if (!np) {
		dev_err(dev, "hibernation_snap[0] node not found\n");
		return -ENODEV;
	}

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem) {
		dev_err(dev, "hibernation_snap[0] reserved_mem lookup failed\n");
		return -ENODEV;
	}

	if (rmem->size < HIBER_SNAP_MISC_OFFSET + HIBER_SNAP_MISC_SIZE) {
		dev_err(dev, "hibernation_snap[0] too small: %pa < 0x%lx\n",
			&rmem->size, HIBER_SNAP_MISC_OFFSET + HIBER_SNAP_MISC_SIZE);
		return -EINVAL;
	}

	spacemit_rproc_ctx.snap_srrpi_va =
		memremap(rmem->base + HIBER_SNAP_MISC_OFFSET, HIBER_SNAP_MISC_SIZE,
			 MEMREMAP_WB);
	if (!spacemit_rproc_ctx.snap_srrpi_va) {
		dev_err(dev, "memremap hibernation_snap_misc failed\n");
		return -ENOMEM;
	}
	spacemit_rproc_ctx.snap_srrpi_size = HIBER_SNAP_MISC_SIZE;
	return 0;
}

static void spacemit_rproc_regions_free(void)
{
	if (spacemit_rproc_ctx.snap_backup) {
		vfree(spacemit_rproc_ctx.snap_backup);
		spacemit_rproc_ctx.snap_backup = NULL;
	}

	if (spacemit_rproc_ctx.snap_rcop_va) {
		memunmap(spacemit_rproc_ctx.snap_rcop_va);
		spacemit_rproc_ctx.snap_rcop_va = NULL;
		spacemit_rproc_ctx.snap_rcop_size = 0;
	}

	if (spacemit_rproc_ctx.rcpu0_boot_entry_va) {
		iounmap(spacemit_rproc_ctx.rcpu0_boot_entry_va);
		spacemit_rproc_ctx.rcpu0_boot_entry_va = NULL;
	}

	if (spacemit_rproc_ctx.hiber_apuse_va) {
		iounmap(spacemit_rproc_ctx.hiber_apuse_va);
		spacemit_rproc_ctx.hiber_apuse_va = NULL;
	}

	if (spacemit_rproc_ctx.snap_srrpi_va) {
		memunmap(spacemit_rproc_ctx.snap_srrpi_va);
		spacemit_rproc_ctx.snap_srrpi_va = NULL;
		spacemit_rproc_ctx.snap_srrpi_size = 0;
	}
}
#endif /* CONFIG_HIBERNATION */

/* ========================================================================
 * Section 4: Platform driver
 * ======================================================================== */

static int spacemit_mbox_init_one(struct device *dev, struct spacemit_mbox *mb)
{
	struct mbox_client *cl = &mb->client;

	cl->dev = dev;
	cl->rx_callback = k3_rproc_mb_callback;
	cl->tx_block = true;
	init_completion(&mb->mb_comp);

	mb->chan = mbox_request_channel_byname(cl, mb->name);
	if (IS_ERR(mb->chan)) {
		dev_err(dev, "failed to request mbox channel '%s'\n", mb->name);
		return PTR_ERR(mb->chan);
	}

	mb->mb_thread = kthread_run(__process_theread, cl, mb->name);
	if (IS_ERR(mb->mb_thread)) {
		int ret = PTR_ERR(mb->mb_thread);

		mbox_free_channel(mb->chan);
		mb->chan = NULL;
		return ret;
	}

	return 0;
}

static int spacemit_rproc_probe(struct platform_device *pdev)
{
	int ret, i;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const char *fw_name;
	struct spacemit_rproc *priv;
	struct rproc *rproc;

	if (!of_node_name_eq(np, "rcpu_rproc0")) {
#ifdef CONFIG_HIBERNATION
		ret = spacemit_rproc_snap_rcop_init(dev);
		if (ret)
			return ret;
		ret = spacemit_rproc_hiber_apuse_init(dev);
		if (ret) {
			memunmap(spacemit_rproc_ctx.snap_rcop_va);
			spacemit_rproc_ctx.snap_rcop_va = NULL;
			return ret;
		}
		ret = spacemit_rproc_snap_srrpi_init(dev);
		if (ret) {
			spacemit_rproc_regions_free();
			return ret;
		}
		spacemit_rproc_ctx.rcpu0_boot_entry_va =
			ioremap(RCPU_CORE0_BOOT_ENTRY_LO, RCPU_BOOT_ENTRY_SIZE);
		if (!spacemit_rproc_ctx.rcpu0_boot_entry_va) {
			dev_err(dev, "ioremap rcpu0 boot entry failed\n");
			spacemit_rproc_regions_free();
			return -ENOMEM;
		}
		/* snap_backup must be allocated before syscore ops are registered so
		 * syscore_suspend never sees a NULL destination pointer. */
		spacemit_rproc_ctx.snap_backup =
			vmalloc(spacemit_rproc_ctx.snap_rcop_size +
				spacemit_rproc_ctx.snap_srrpi_size);
		if (!spacemit_rproc_ctx.snap_backup) {
			dev_err(dev, "failed to alloc snap_backup (%zu bytes)\n",
				spacemit_rproc_ctx.snap_rcop_size +
				spacemit_rproc_ctx.snap_srrpi_size);
			spacemit_rproc_regions_free();
			return -ENOMEM;
		}
		register_pm_notifier(&spacemit_rproc_pm_nb);
		register_syscore_ops_first(&spacemit_rcpu0_syscore_ops);
#endif /* CONFIG_HIBERNATION */
	}

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret < 0 && ret != -EINVAL)
		goto err_hiber;

	rproc = devm_rproc_alloc(dev, np->name, &spacemit_rproc_ops,
				 fw_name, sizeof(*priv));
	if (!rproc) {
		ret = -ENOMEM;
		goto err_hiber;
	}

	priv = rproc->priv;
	priv->dev = dev;
	platform_set_drvdata(pdev, rproc);

	priv->mb[0].name = "vq0";
	priv->mb[0].vq_id = K3_MBOX_VQ0_ID;
	priv->mb[1].name = "vq1";
	priv->mb[1].vq_id = K3_MBOX_VQ1_ID;

	for (i = 0; i < MAX_MBOX; ++i) {
		ret = spacemit_mbox_init_one(dev, &priv->mb[i]);
		if (ret)
			goto err_mbox;
	}

	rproc->auto_boot = true;
	rproc->state = RPROC_DETACHED;
	ret = devm_rproc_add(dev, rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed\n");
		goto err_mbox;
	}

	return 0;

err_mbox:
	while (--i >= 0) {
		kthread_stop(priv->mb[i].mb_thread);
		mbox_free_channel(priv->mb[i].chan);
	}
err_hiber:
#ifdef CONFIG_HIBERNATION
	if (!of_node_name_eq(np, "rcpu_rproc0")) {
		unregister_syscore_ops(&spacemit_rcpu0_syscore_ops);
		unregister_pm_notifier(&spacemit_rproc_pm_nb);
		spacemit_rproc_regions_free();
	}
#endif
	return ret;
}

static void k3_rproc_free_mbox(struct rproc *rproc)
{
	struct spacemit_rproc *ddata = rproc->priv;
	unsigned int i;

	for (i = 0; i < MAX_MBOX; i++) {
		if (ddata->mb[i].chan)
			mbox_free_channel(ddata->mb[i].chan);
		ddata->mb[i].chan = NULL;
	}
}

static void spacemit_rproc_remove(struct platform_device *pdev)
{
	int i = 0;
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct device_node *np = pdev->dev.of_node;
	struct spacemit_rproc *ddata;

	if (!rproc)
		return;

	ddata = rproc->priv;

	for (i = 0; i < MAX_MBOX; ++i)
		if (ddata->mb[i].mb_thread)
			kthread_stop(ddata->mb[i].mb_thread);

	rproc_del(rproc);
	k3_rproc_free_mbox(rproc);

	if (ddata->rsc_table_va) {
		iounmap(ddata->rsc_table_va);
		ddata->rsc_table_va = NULL;
	}
	kfree(ddata->rsc_table_ptr);
	ddata->rsc_table_ptr = NULL;

#ifdef CONFIG_HIBERNATION
	if (!of_node_name_eq(np, "rcpu_rproc0")) {
		unregister_syscore_ops(&spacemit_rcpu0_syscore_ops);
		unregister_pm_notifier(&spacemit_rproc_pm_nb);
		spacemit_rproc_regions_free();
	}
#endif
}

static void spacemit_rproc_shutdown(struct platform_device *pdev)
{
	int i;
	struct rproc *rproc;
	struct spacemit_rproc *priv;

	rproc = dev_get_drvdata(&pdev->dev);
	if (!rproc)
		return;

	priv = rproc->priv;

	for (i = 0; i < MAX_MBOX; ++i) {
		/* release the resource of rt thread */
		if (priv->mb[i].mb_thread) {
			if (!frozen((priv->mb[i].mb_thread)))
				kthread_stop(priv->mb[i].mb_thread);
		}
		/* mbox_free_channel(priv->mb[i].chan); */
	}
}

static const struct of_device_id spacemit_rproc_of_match[] = {
	{ .compatible = "spacemit,k3-rproc" },
	{},
};

MODULE_DEVICE_TABLE(of, spacemit_rproc_of_match);

static struct platform_driver spacemit_rproc_driver = {
	.probe = spacemit_rproc_probe,
	.remove = spacemit_rproc_remove,
	.shutdown = spacemit_rproc_shutdown,
	.driver = {
		.name = "spacemit-rproc",
		.of_match_table = spacemit_rproc_of_match,
	},
};

static __init int spacemit_rproc_driver_init(void)
{
	return platform_driver_register(&spacemit_rproc_driver);
}
device_initcall(spacemit_rproc_driver_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("sapcemit remote processor control driver");
