// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpufreq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpumask.h>
#include <linux/clk/clk-conf.h>
#include <linux/pm_qos.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include "../opp/opp.h"
#include "cpufreq-dt.h"

#define TURBO0_FREQUENCY		(1000000000)
#define STABLE_FREQUENCY		(819200000)

/*
 * OPP table index selection by PMIC solution:
 *   index 0 — spacemit,tda38740  (X100 rated 0.95 V, default)
 *   index 1 — spacemit,au4562   (X100 rated 0.90 V)
 *
 * The U-Boot ft_board_cpu_fixup() writes "pmic-compatible" into /cpus
 * from the board EEPROM TLV_CODE_PMIC_TYPE field.  We read that here.
 */

static int spacemit_processor_notifier(struct notifier_block *nb,
                                  unsigned long event, void *data)
{
	int cpu;
	struct device *cpu_dev;
	struct cpufreq_freqs *freqs = (struct cpufreq_freqs *)data;
	struct cpufreq_policy *policy = ( struct cpufreq_policy *)freqs->policy;
	struct opp_table *opp_table;
	struct clk *pll_clst0, *pll_clst1, *pll_src, *clt_pll_src;
	int i;

	cpu = cpumask_first(policy->related_cpus);
	cpu_dev = get_cpu_device(cpu);
	opp_table = _find_opp_table(cpu_dev);

	/* get the pll clk handler */
	pll_clst0 = of_clk_get_by_name(opp_table->np, "pll_clst0");
	pll_clst1 = of_clk_get_by_name(opp_table->np, "pll_clst1");
	pll_src = of_clk_get_by_name(opp_table->np, "pll_src");
	clt_pll_src = of_clk_get_by_name(opp_table->np, "clt_pll_src");

	if (event == CPUFREQ_PRECHANGE) {

		if (freqs->new * 1000 > TURBO0_FREQUENCY) {
			if (freqs->old * 1000 > TURBO0_FREQUENCY) {
				for (i = 0; i < opp_table->clk_count; ++i)
					clk_set_rate(opp_table->clks[i], STABLE_FREQUENCY);
			}

			if (freqs->new * 1000 > TURBO0_FREQUENCY) {
				/* set pll_clst0/1 to target frequency */
				if (!IS_ERR(pll_clst0))
					clk_set_rate(pll_clst0, freqs->new * 1000);

				if (!IS_ERR(pll_clst1))
					clk_set_rate(pll_clst1, freqs->new * 1000);
			}
		}
	}

	i = clk_set_parent(clt_pll_src, pll_src);

	if (event == CPUFREQ_POSTCHANGE) {
		/* TODO */
	}

	if (!IS_ERR(pll_clst0))
		clk_put(pll_clst0);
	if (!IS_ERR(pll_clst1))
		clk_put(pll_clst1);

	dev_pm_opp_put_opp_table(opp_table);

	return 0;
}

static struct notifier_block spacemit_processor_notifier_block = {
       .notifier_call = spacemit_processor_notifier,
};

static int spacemit_policy_notifier(struct notifier_block *nb,
                                  unsigned long event, void *data)
{
	int cpu;
	struct device *cpu_dev;
	struct cpufreq_policy *policy = data;
	struct opp_table *opp_table;

	cpu = cpumask_first(policy->related_cpus);
	cpu_dev = get_cpu_device(cpu);
	opp_table = _find_opp_table(cpu_dev);

	if (policy->clk)
		clk_put(policy->clk);

	/* cover the policy->clk & opp_table->clk which has been set before */
	policy->clk = opp_table->clks[0];
	opp_table->clk = opp_table->clks[0];

	return 0;
}

static struct notifier_block spacemit_policy_notifier_block = {
       .notifier_call = spacemit_policy_notifier,
};

/*
 * Custom indexed OPP sharing helpers — mirrors the K1X approach so each
 * CPU can carry multiple operating-points-v2 phandles and the driver picks
 * the right one at boot based on PMIC type.
 */
static int _dev_pm_opp_of_get_sharing_cpus(struct device *cpu_dev,
				   struct cpumask *cpumask, int index)
{
	struct device_node *np, *tmp_np, *cpu_np;
	int cpu, ret = 0;

	np = of_parse_phandle(cpu_dev->of_node, "operating-points-v2", index);
	if (!np) {
		dev_dbg(cpu_dev, "%s: Couldn't find opp node.\n", __func__);
		return -ENOENT;
	}

	cpumask_set_cpu(cpu_dev->id, cpumask);

	if (!of_property_read_bool(np, "opp-shared"))
		goto put_cpu_node;

	for_each_possible_cpu(cpu) {
		if (cpu == cpu_dev->id)
			continue;

		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_err(cpu_dev, "%s: failed to get cpu%d node\n",
				__func__, cpu);
			ret = -ENOENT;
			goto put_cpu_node;
		}

		tmp_np = of_parse_phandle(cpu_np, "operating-points-v2", index);
		of_node_put(cpu_np);
		if (!tmp_np)
			continue;

		if (np == tmp_np)
			cpumask_set_cpu(cpu, cpumask);

		of_node_put(tmp_np);
	}

put_cpu_node:
	of_node_put(np);
	return ret;
}

static int _dev_pm_opp_of_cpumask_add_table(const struct cpumask *cpumask, int index)
{
	struct device *cpu_dev;
	int cpu, ret;

	if (WARN_ON(cpumask_empty(cpumask)))
		return -ENODEV;

	for_each_cpu(cpu, cpumask) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev) {
			pr_err("%s: failed to get cpu%d device\n", __func__, cpu);
			ret = -ENODEV;
			goto remove_table;
		}

		ret = dev_pm_opp_of_add_table_indexed(cpu_dev, index);
		if (ret) {
			pr_debug("%s: couldn't find opp table for cpu:%d, %d\n",
				 __func__, cpu, ret);
			goto remove_table;
		}
	}

	return 0;

remove_table:
	_dev_pm_opp_cpumask_remove_table(cpumask, cpu);
	return ret;
}

static int spacemit_dt_cpufreq_pre_early_init(struct device *dev, int cpu, int index)
{
	struct private_data *priv;
	struct device *cpu_dev;
	struct opp_table *opp_table;
	const char *reg_name[] = { "clst", NULL };
	const char *clk_name[] = { "cls0", "cls1", NULL };
	struct dev_pm_opp_config config = {
		.regulator_names = reg_name,
		.clk_names = clk_name,
		.config_clks = dev_pm_opp_config_clks_simple,
	};
	int ret;

	/* Check if this CPU is already covered by some other policy */
	if (cpufreq_dt_find_data(cpu))
		return 0;

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev)
		return -EPROBE_DEFER;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (!alloc_cpumask_var(&priv->cpus, GFP_KERNEL))
		return -ENOMEM;

	cpumask_set_cpu(cpu, priv->cpus);
	priv->cpu_dev = cpu_dev;

	/* A100 cluster (cpu8+) has no voltage regulator */
	if (cpu >= 8)
		config.regulator_names = NULL;

	/*
	 * dev_pm_opp_set_config() always looks up opp_table at index=0,
	 * so opp_table->np would point to the wrong DT node when index>0.
	 * Pre-create the table with the correct index first so that np is
	 * set correctly; dev_pm_opp_set_config() will reuse the existing
	 * table via _find_opp_table_unlocked().
	 */
	/*
	 * dev_pm_opp_set_config() always looks up opp_table at index=0,
	 * so opp_table->np would point to the wrong DT node when index>0.
	 * Pre-create the table with the correct index first so that np is
	 * set correctly; keep the reference alive until after set_config()
	 * so the table is not freed before set_config finds it via
	 * _find_opp_table_unlocked().
	 */
	opp_table = _add_opp_table_indexed(cpu_dev, index, false);
	if (IS_ERR(opp_table)) {
		ret = PTR_ERR(opp_table);
		goto free_cpumask;
	}

	priv->opp_token = dev_pm_opp_set_config(cpu_dev, &config);
	dev_pm_opp_put_opp_table(opp_table);
	if (priv->opp_token < 0) {
		ret = -EPROBE_DEFER;
		goto free_cpumask;
	}

	ret = _dev_pm_opp_of_get_sharing_cpus(cpu_dev, priv->cpus, index);
	if (ret)
		goto out;

	ret = _dev_pm_opp_of_cpumask_add_table(priv->cpus, index);
	if (!ret) {
		priv->have_static_opps = true;
	} else if (ret == -EPROBE_DEFER) {
		goto out;
	}

	ret = dev_pm_opp_get_opp_count(cpu_dev);
	if (ret <= 0) {
		dev_err(cpu_dev, "OPP table can't be empty\n");
		ret = -ENODEV;
		goto out;
	}

	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &priv->freq_table);
	if (ret) {
		dev_err(cpu_dev, "failed to init cpufreq table: %d\n", ret);
		goto out;
	}

	cpufreq_dt_add_data(priv);

	return 0;

out:
	if (priv->have_static_opps)
		dev_pm_opp_of_cpumask_remove_table(priv->cpus);
	dev_pm_opp_put_regulators(priv->opp_token);
free_cpumask:
	free_cpumask_var(priv->cpus);
	return ret;
}

static struct cpufreq_dt_platform_data spacemit_cpufreq_dt_pdata = {
	.have_governor_per_policy = true,
};

static int spacemit_dt_cpufreq_pre_probe(struct platform_device *pdev)
{
	int cpu, ret = 0;
	int x100_index = 0;
	struct device_node *cpus_np;
	const char *pmic_compat = NULL;

	if (strncmp(pdev->name, "cpufreq-dt", 10) != 0)
		return 0;

	pdev->dev.platform_data = &spacemit_cpufreq_dt_pdata;

	cpus_np = of_find_node_by_path("/cpus");
	if (cpus_np) {
		of_property_read_string(cpus_np, "pmic-compatible", &pmic_compat);
		of_node_put(cpus_np);
	}

	if (pmic_compat && strcmp(pmic_compat, "spacemit,au4562") == 0)
		x100_index = 1;

	pr_info("Spacemit K3: pmic-compatible=\"%s\", using X100 OPP table%d\n",
		pmic_compat ? pmic_compat : "(none)", x100_index);

	for_each_possible_cpu(cpu) {
		/* A100 cluster (cpu8+) only has a single OPP table */
		int cpu_index = (cpu >= 8) ? 0 : x100_index;

		ret = spacemit_dt_cpufreq_pre_early_init(&pdev->dev, cpu, cpu_index);
		if (ret)
			pr_err("Spacemit K3: cpu%d OPP init failed (%d)\n", cpu, ret);
	}

	return 0;
}

static int __device_notifier_call(struct notifier_block *nb,
				      unsigned long event, void *dev)
{
	struct platform_device *pdev = to_platform_device(dev);

	switch (event) {
	case BUS_NOTIFY_REMOVED_DEVICE:
		break;
	case BUS_NOTIFY_UNBOUND_DRIVER:
		break;
	case BUS_NOTIFY_BIND_DRIVER:
		spacemit_dt_cpufreq_pre_probe(pdev);
		break;
	case BUS_NOTIFY_ADD_DEVICE:
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block spacemit_platform_nb = {
	.notifier_call = __device_notifier_call,
};

static int __init spacemit_processor_driver_init(void)
{
       int ret;

	ret = cpufreq_register_notifier(&spacemit_processor_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
	if (ret) {
		pr_err("register cpufreq notifier failed\n");
		return -EINVAL;
	}

       ret = cpufreq_register_notifier(&spacemit_policy_notifier_block, CPUFREQ_POLICY_NOTIFIER);
       if (ret) {
               pr_err("register cpufreq notifier failed\n");
               return -EINVAL;
       }

	bus_register_notifier(&platform_bus_type, &spacemit_platform_nb);

       return 0;
}
arch_initcall(spacemit_processor_driver_init);
