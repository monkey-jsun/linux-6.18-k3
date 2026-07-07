// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(C) 2026 Spacemit Limited. All rights reserved.
 * Author: liangzhen <zhen.liang@spacemit.com>
 */

#include <linux/delay.h>
#include <linux/percpu.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/rvtrace.h>

int rvtrace_poll_bit(struct rvtrace_component *comp, int offset,
                     int bit, int bitval)
{
	int i = RVTRACE_TIMEOUT_US;
	u32 val;

	while (i--) {
		val = readl_relaxed(comp->base + offset);
		if (((val >> bit) & 0x1) == bitval)
			break;
		udelay(1);
	}

	return (i < 0) ? -ETIMEDOUT : 0;
}
EXPORT_SYMBOL_GPL(rvtrace_poll_bit);

int rvtrace_enable_component(struct rvtrace_component *comp)
{
	u32 val;

	val = readl_relaxed(comp->base + RVTRACE_COMPONENT_CTRL_OFFSET);
	val |= BIT(RVTRACE_COMPONENT_CTRL_ENABLE_SHIFT);
	writel_relaxed(val, comp->base + RVTRACE_COMPONENT_CTRL_OFFSET);
	return rvtrace_poll_bit(comp, RVTRACE_COMPONENT_CTRL_OFFSET,
				RVTRACE_COMPONENT_CTRL_ENABLE_SHIFT, 1);
}
EXPORT_SYMBOL_GPL(rvtrace_enable_component);

int rvtrace_disable_component(struct rvtrace_component *comp)
{
	u32 val;

	val = readl_relaxed(comp->base + RVTRACE_COMPONENT_CTRL_OFFSET);
	val &= ~BIT(RVTRACE_COMPONENT_CTRL_ENABLE_SHIFT);
	writel_relaxed(val, comp->base + RVTRACE_COMPONENT_CTRL_OFFSET);
	return rvtrace_poll_bit(comp, RVTRACE_COMPONENT_CTRL_OFFSET,
				RVTRACE_COMPONENT_CTRL_ENABLE_SHIFT, 0);
}
EXPORT_SYMBOL_GPL(rvtrace_disable_component);

int rvtrace_component_reset(struct rvtrace_component *comp)
{
	int ret;

	writel_relaxed(0, comp->base + RVTRACE_COMPONENT_CTRL_OFFSET);
	ret = rvtrace_poll_bit(comp, RVTRACE_COMPONENT_CTRL_OFFSET,
			       RVTRACE_COMPONENT_CTRL_ACTIVE_SHIFT, 0);
	if (ret)
		return ret;

	writel_relaxed(RVTRACE_COMPONENT_CTRL_ACTIVE_MASK,
			comp->base + RVTRACE_COMPONENT_CTRL_OFFSET);
	return rvtrace_poll_bit(comp, RVTRACE_COMPONENT_CTRL_OFFSET,
				RVTRACE_COMPONENT_CTRL_ACTIVE_SHIFT, 1);
}
EXPORT_SYMBOL_GPL(rvtrace_component_reset);

bool rvtrace_loses_context_with_cpu(struct device *dev)
{
	return fwnode_property_present(dev_fwnode(dev),
				       "riscv,rvtrace-loses-context-with-cpu");
}
EXPORT_SYMBOL_GPL(rvtrace_loses_context_with_cpu);

static void rvtrace_component_id_smp_call(void *info)
{
	u32 impl, type, major, minor;
	struct component_arg *arg = info;
	struct rvtrace_component *comp = arg->comp;

	arg->rc = rvtrace_component_reset(comp);
	if (arg->rc)
		return;
	comp->was_reset = true;

	impl = readl_relaxed(comp->base + RVTRACE_COMPONENT_IMPL_OFFSET);
	type = (impl >> RVTRACE_COMPONENT_IMPL_TYPE_SHIFT) &
		RVTRACE_COMPONENT_IMPL_TYPE_MASK;
	major = (impl >> RVTRACE_COMPONENT_IMPL_VERMAJOR_SHIFT) &
		RVTRACE_COMPONENT_IMPL_VERMAJOR_MASK;
	minor = (impl >> RVTRACE_COMPONENT_IMPL_VERMINOR_SHIFT) &
		RVTRACE_COMPONENT_IMPL_VERMINOR_MASK;

	comp->id.type = type;
	comp->id.version = rvtrace_component_mkversion(major, minor);
}

struct rvtrace_component *rvtrace_register_component(struct platform_device *pdev)
{
	int ret;
	void __iomem *base;
	struct device *dev = &pdev->dev;
	struct rvtrace_component *comp;
	struct resource *res;
	struct device_node *node;
	struct component_arg arg = { };

	comp = devm_kzalloc(dev, sizeof(*comp), GFP_KERNEL);
	if (!comp) {
		ret = -ENOMEM;
		goto err_out;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base)) {
		ret = -EINVAL;
		goto err_out;
	}
	comp->base = base;

	comp->cpu = -1;
	node = of_parse_phandle(dev->of_node, "cpus", 0);
	if (node) {
		ret = of_cpu_node_to_id(node);
		of_node_put(node);
		if (ret < 0) {
			dev_err(dev, "failed to get CPU id for %pOF\n", node);
			ret = -EINVAL;
			goto err_out;
		}
		comp->cpu = ret;
	}

	if (comp->cpu >= 0 && !cpu_online(comp->cpu)) {
		dev_err(dev, "No valid CPU found in 'cpus' property\n");
		ret = -EINVAL;
		goto err_out;
	}

	cpus_read_lock();
	arg.comp = comp;
	if (comp->cpu >= 0) {
		ret = smp_call_function_single(comp->cpu, rvtrace_component_id_smp_call, &arg, 1);
		if (!ret)
			ret = arg.rc;
		if (ret) {
			cpus_read_unlock();
			goto err_out;
		}
	} else {
		rvtrace_component_id_smp_call(&arg);
		if (arg.rc) {
			ret = arg.rc;
			cpus_read_unlock();
			goto err_out;
		}
	}
	cpus_read_unlock();

	if (comp->id.type == RVTRACE_COMPONENT_TYPE_ENCODER && comp->cpu < 0) {
		ret = -EINVAL;
		goto err_out;
	}

	return comp;

err_out:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(rvtrace_register_component);
