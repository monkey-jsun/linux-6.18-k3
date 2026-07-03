// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(C) 2026 Spacemit Limited. All rights reserved.
 * Author: liangzhen <zhen.liang@spacemit.com>
 */

#include <linux/sysfs.h>
#include <linux/rvtrace.h>
#include "rvtrace-encoder.h"
#include "rvtrace-timestamp.h"
#include "coresight-priv.h"

static void do_smp_cross_read(void *data)
{
	struct component_reg *reg = data;
	reg->data = readl_relaxed(reg->comp->base + reg->offset);
}

static u32 rvtrace_cross_read(struct rvtrace_component *comp, u32 offset)
{
	struct component_reg reg;

	reg.comp = comp;
	reg.offset = offset;
	/*
	 * smp cross call ensures the CPU will be powered up before
	 * accessing the RISC-V trace core registers
	 */
	smp_call_function_single(comp->cpu, do_smp_cross_read, &reg, 1);
	return reg.data;
}

static u32 rvtrace_attr_to_offset(struct device_attribute *attr)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);
	return (u32)(unsigned long)eattr->var;
}

static ssize_t rvtrace_reg_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	u32 val, offset;
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);

	offset = rvtrace_attr_to_offset(attr);

	val = rvtrace_cross_read(comp, offset);

	return scnprintf(buf, PAGE_SIZE, "0x%x\n", val);
}

/*
 * Macro to set an RO ext attribute with offset and show function.
 */
#define rvtrace_reg_showfn(name, offset, showfn) (	    \
	&((struct dev_ext_attribute[]) {		    \
	   {						    \
		__ATTR(name, 0444, showfn, NULL),	    \
		(void *)(unsigned long)offset		    \
	   }						    \
	})[0].attr.attr					    \
	)

/* macro using the default rvtrace_reg_show function */
#define rvtrace_reg(name, offset)	\
	rvtrace_reg_showfn(name, offset, rvtrace_reg_show)


static ssize_t cpu_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	unsigned long val;
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);

	val = comp->cpu;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}
static DEVICE_ATTR_RO(cpu);

static void rvtrace_component_reset_smp_call(void *info)
{
	struct component_arg *arg = info;
	struct rvtrace_component *comp = arg->comp;

	arg->rc = rvtrace_component_reset(comp);
}

static ssize_t reset_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t size)
{
	int ret;
	unsigned long val;
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);
	struct encoder_data *encoder_data = rvtrace_component_data(comp);
	struct component_arg arg = { };

	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	arg.comp = comp;

	spin_lock(&encoder_data->spinlock);

	if (val) {
		ret = smp_call_function_single(comp->cpu, rvtrace_component_reset_smp_call, &arg, 1);
		if (!ret)
			ret = arg.rc;
		if (ret) {
			comp->was_reset = false;
			spin_unlock(&encoder_data->spinlock);
			return -EINVAL;
		}
		encoder_set_default(comp);
	}

	spin_unlock(&encoder_data->spinlock);

	return size;
}
static DEVICE_ATTR_WO(reset);

static ssize_t ts_ctrl_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);
	struct encoder_data *encoder_data = rvtrace_component_data(comp);

	return scnprintf(buf, PAGE_SIZE, "%u\n", encoder_data->ts_ctrl);
}

static ssize_t ts_ctrl_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t size)
{
	unsigned long val;
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);
	struct encoder_data *encoder_data = rvtrace_component_data(comp);

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	spin_lock(&encoder_data->spinlock);
	encoder_data->ts_ctrl = !!val;
	spin_unlock(&encoder_data->spinlock);

	return size;
}
static DEVICE_ATTR_RW(ts_ctrl);

static struct attribute *trace_encoder_attrs[] = {
	&dev_attr_cpu.attr,
	&dev_attr_reset.attr,
	&dev_attr_ts_ctrl.attr,
	NULL,
};

static struct attribute *trace_encoder_mgmt_attrs[] = {
	rvtrace_reg(control, RVTRACE_COMPONENT_CTRL_OFFSET),
	rvtrace_reg(impl, RVTRACE_COMPONENT_IMPL_OFFSET),
	rvtrace_reg(features, RVTRACE_ENCODER_INST_FTRS_OFFSET),
	NULL,
};

#define encoder_simple_rw(name)						    \
static ssize_t name##_show(struct device *dev,				    \
			   struct device_attribute *attr, char *buf)	    \
{									    \
	unsigned long val;						    \
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);	    \
	struct encoder_data *encoder_data = rvtrace_component_data(comp);   \
	struct encoder_config *config = &encoder_data->config;		    \
									    \
	val = config->name;						    \
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);		    \
}									    \
									    \
static ssize_t name##_store(struct device *dev,				    \
			    struct device_attribute *attr,		    \
			    const char *buf, size_t size)		    \
{									    \
	unsigned long val;						    \
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);	    \
	struct encoder_data *encoder_data = rvtrace_component_data(comp);   \
	struct encoder_config *config = &encoder_data->config;		    \
									    \
	if (kstrtoul(buf, 16, &val))					    \
		return -EINVAL;						    \
									    \
	spin_lock(&encoder_data->spinlock);	    			    \
	config->name = val;						    \
	spin_unlock(&encoder_data->spinlock);				    \
									    \
	return size;							    \
}									    \
static DEVICE_ATTR_RW(name);

static const char *const instmodes_str[] = {
	[ENCODER_INSTMODE_OFF]	    = "off",
	[ENCODER_INSTMODE_RESV1]    = "resv1",
	[ENCODER_INSTMODE_BTM]	    = "btm",
	[ENCODER_INSTMODE_RESV4]    = "resv4",
	[ENCODER_INSTMODE_RESV5]    = "resv5",
	[ENCODER_INSTMODE_HTM]	    = "htm",
	[ENCODER_INSTMODE_RESV7]    = "resv7",
};

static ssize_t instmode_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	unsigned long val;
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);

	val = rvtrace_cross_read(comp, RVTRACE_COMPONENT_CTRL_OFFSET);
	val = BMVAL(val, 4, 6);

	return sysfs_emit(buf, "%s\n", instmodes_str[val]);
}
static DEVICE_ATTR_RO(instmode);

static const char *const inst_syncmodes_str[] = {
	[ENCODER_SYNCMODE_OFF]		= "off",
	[ENCODER_SYNCMODE_MESSAGES]	= "messages",
	[ENCODER_SYNCMODE_CLOCK]	= "clock",
	[ENCODER_SYNCMODE_INSTRUCTION]	= "instruction"
};

static ssize_t inst_syncmodes_available_show(struct device *dev,
					     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s %s %s %s\n",
			    inst_syncmodes_str[ENCODER_SYNCMODE_OFF],
			    inst_syncmodes_str[ENCODER_SYNCMODE_MESSAGES],
			    inst_syncmodes_str[ENCODER_SYNCMODE_CLOCK],
			    inst_syncmodes_str[ENCODER_SYNCMODE_INSTRUCTION]);
}
static DEVICE_ATTR_RO(inst_syncmodes_available);

static ssize_t inst_syncmode_preferred_show(struct device *dev,
					    struct device_attribute *attr, char *buf)
{
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);
	struct encoder_data *encoder_data = rvtrace_component_data(comp);
	struct encoder_config *config = &encoder_data->config;

        return sysfs_emit(buf, "%s\n", inst_syncmodes_str[config->inst_syncmode]);
}

static ssize_t inst_syncmode_preferred_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t size)
{
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);
	struct encoder_data *encoder_data = rvtrace_component_data(comp);
	struct encoder_config *config = &encoder_data->config;

	if (sysfs_streq(buf, inst_syncmodes_str[ENCODER_SYNCMODE_OFF])) {
		config->inst_syncmode = ENCODER_SYNCMODE_OFF;
	} else if (sysfs_streq(buf, inst_syncmodes_str[ENCODER_SYNCMODE_MESSAGES])) {
		config->inst_syncmode = ENCODER_SYNCMODE_MESSAGES;
	} else if (sysfs_streq(buf, inst_syncmodes_str[ENCODER_SYNCMODE_CLOCK])) {
		config->inst_syncmode = ENCODER_SYNCMODE_CLOCK;
	} else if (sysfs_streq(buf, inst_syncmodes_str[ENCODER_SYNCMODE_INSTRUCTION])) {
		config->inst_syncmode = ENCODER_SYNCMODE_INSTRUCTION;
	} else {
		return -EINVAL;
	}
	return size;
}
static DEVICE_ATTR_RW(inst_syncmode_preferred);

static ssize_t format_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	unsigned long val;
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);

	val = rvtrace_cross_read(comp, RVTRACE_COMPONENT_CTRL_OFFSET);
	val = BMVAL(val, 24, 26);

	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}
static DEVICE_ATTR_RO(format);

encoder_simple_rw(context);
encoder_simple_rw(inst_trigger);
encoder_simple_rw(inst_stall);
encoder_simple_rw(inhb_src);
encoder_simple_rw(inst_syncmax);

static struct attribute *trace_encoder_control_attrs[] = {
	&dev_attr_instmode.attr,
	&dev_attr_context.attr,
	&dev_attr_inst_trigger.attr,
	&dev_attr_inst_stall.attr,
	&dev_attr_inhb_src.attr,
	&dev_attr_inst_syncmodes_available.attr,
	&dev_attr_inst_syncmode_preferred.attr,
	&dev_attr_inst_syncmax.attr,
	&dev_attr_format.attr,
	NULL,
};

static const char *const inst_implicit_return_modes_str[] = {
	[ENCODER_IMPLICITRETURNMODE_NOT_SUPPORTED]	= "off",
	[ENCODER_IMPLICITRETURNMODE_SIMPLE_COUNTING]	= "simple",
	[ENCODER_IMPLICITRETURNMODE_PARTIAL_ADDRESS]	= "partial",
	[ENCODER_IMPLICITRETURNMODE_FULL_ADDRESS]	= "full"
};

static ssize_t inst_implicit_return_modes_available_show(struct device *dev,
							 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s %s %s %s\n",
			    inst_implicit_return_modes_str[ENCODER_IMPLICITRETURNMODE_NOT_SUPPORTED],
			    inst_implicit_return_modes_str[ENCODER_IMPLICITRETURNMODE_SIMPLE_COUNTING],
			    inst_implicit_return_modes_str[ENCODER_IMPLICITRETURNMODE_PARTIAL_ADDRESS],
			    inst_implicit_return_modes_str[ENCODER_IMPLICITRETURNMODE_FULL_ADDRESS]);
}
static DEVICE_ATTR_RO(inst_implicit_return_modes_available);


static ssize_t inst_implicit_return_mode_preferred_show(struct device *dev,
					    struct device_attribute *attr, char *buf)
{
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);
	struct encoder_data *encoder_data = rvtrace_component_data(comp);
	struct encoder_config *config = &encoder_data->config;

	return sysfs_emit(buf, "%s\n", inst_implicit_return_modes_str[config->impl_ret_mode]);
}

static ssize_t inst_implicit_return_mode_preferred_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t size)
{
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);
	struct encoder_data *encoder_data = rvtrace_component_data(comp);
	struct encoder_config *config = &encoder_data->config;

	if (sysfs_streq(buf, inst_implicit_return_modes_str[ENCODER_IMPLICITRETURNMODE_NOT_SUPPORTED])) {
		config->impl_ret_mode = ENCODER_IMPLICITRETURNMODE_NOT_SUPPORTED;
	} else if (sysfs_streq(buf, inst_implicit_return_modes_str[ENCODER_IMPLICITRETURNMODE_SIMPLE_COUNTING])) {
		config->impl_ret_mode = ENCODER_IMPLICITRETURNMODE_SIMPLE_COUNTING;
	} else if (sysfs_streq(buf, inst_implicit_return_modes_str[ENCODER_IMPLICITRETURNMODE_PARTIAL_ADDRESS])) {
		config->impl_ret_mode = ENCODER_IMPLICITRETURNMODE_PARTIAL_ADDRESS;
	} else if (sysfs_streq(buf, inst_implicit_return_modes_str[ENCODER_IMPLICITRETURNMODE_FULL_ADDRESS])) {
		config->impl_ret_mode = ENCODER_IMPLICITRETURNMODE_FULL_ADDRESS;
	} else {
		return -EINVAL;
	}
	return size;
}
static DEVICE_ATTR_RW(inst_implicit_return_mode_preferred);

encoder_simple_rw(inst_na_diff);
encoder_simple_rw(inst_nt_addr);
encoder_simple_rw(seq_jump);
encoder_simple_rw(impl_ret);
encoder_simple_rw(branch_pred);
encoder_simple_rw(jump_target_cache);
encoder_simple_rw(rep_hist);
encoder_simple_rw(all_jumps);
encoder_simple_rw(ext_msb);
encoder_simple_rw(srcid);
encoder_simple_rw(srcb);

static struct attribute *trace_encoder_features_attrs[] = {
	&dev_attr_inst_na_diff.attr,
	&dev_attr_inst_nt_addr.attr,
	&dev_attr_seq_jump.attr,
	&dev_attr_impl_ret.attr,
	&dev_attr_branch_pred.attr,
	&dev_attr_jump_target_cache.attr,
	&dev_attr_inst_implicit_return_modes_available.attr,
	&dev_attr_inst_implicit_return_mode_preferred.attr,
	&dev_attr_rep_hist.attr,
	&dev_attr_all_jumps.attr,
	&dev_attr_ext_msb.attr,
	&dev_attr_srcid.attr,
	&dev_attr_srcb.attr,
	NULL,
};

static umode_t timestamp_attr_is_visible(struct kobject *kobj,
					 struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct rvtrace_component *comp = dev_get_drvdata(dev->parent);
	struct encoder_data *encoder_data = rvtrace_component_data(comp);

	if (encoder_data->has_timestamp)
		return attr->mode;

	return 0;
}

static const struct attribute_group trace_encoder_group = {
	.attrs = trace_encoder_attrs,
};

static const struct attribute_group trace_encoder_mgmt_group = {
	.attrs = trace_encoder_mgmt_attrs,
	.name = "mgmt",
};

static const struct attribute_group trace_encoder_control_group = {
	.attrs = trace_encoder_control_attrs,
	.name = "control",
};

static const struct attribute_group trace_encoder_features_group = {
	.attrs = trace_encoder_features_attrs,
	.name = "features",
};

extern const struct attribute *timestamp_attrs[];

static const struct attribute_group trace_encoder_timestamp_group = {
	.attrs = (struct attribute **)timestamp_attrs,
	.name = "timestamp",
	.is_visible = timestamp_attr_is_visible,
};

const struct attribute_group *trace_encoder_groups[] = {
	&trace_encoder_group,
	&trace_encoder_mgmt_group,
	&trace_encoder_control_group,
	&trace_encoder_features_group,
	&trace_encoder_timestamp_group,
	NULL,
};
