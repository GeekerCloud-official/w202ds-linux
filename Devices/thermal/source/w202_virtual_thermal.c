// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2020 Unisoc Inc.
/* Mainline API adaptation of sprd_soc_thm.c, stock virtual-thermal binding.
 * Aggregate the maximum of every referenced zone; retain DT trips/maps.
 */
#include <linux/device.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

struct w202_virtual_thermal {
	unsigned int count;
	struct thermal_zone_device **sources;
};

static int w202_virtual_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct w202_virtual_thermal *data = thermal_zone_device_priv(tz);
	int maximum = INT_MIN;
	unsigned int i;

	for (i = 0; i < data->count; i++) {
		int reading, ret;

		ret = thermal_zone_get_temp(data->sources[i], &reading);
		if (ret)
			return ret;
		maximum = max(maximum, reading);
	}
	*temp = maximum;
	return 0;
}

static const struct thermal_zone_device_ops w202_virtual_ops = {
	.get_temp = w202_virtual_get_temp,
};

static ssize_t sources_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct w202_virtual_thermal *data = dev_get_drvdata(dev);
	int maximum = INT_MIN;
	unsigned int i;
	ssize_t len = 0;

	for (i = 0; i < data->count; i++) {
		int reading, ret;

		ret = thermal_zone_get_temp(data->sources[i], &reading);
		if (ret)
			return ret;
		maximum = max(maximum, reading);
		len += sysfs_emit_at(buf, len, "%s=%d\n",
			thermal_zone_device_type(data->sources[i]), reading);
	}
	return len + sysfs_emit_at(buf, len, "maximum=%d count=%u\n",
				   maximum, data->count);
}
static DEVICE_ATTR_RO(sources);
static struct attribute *w202_virtual_attrs[] = {
	&dev_attr_sources.attr,
	NULL,
};
static const struct attribute_group w202_virtual_group = {
	.attrs = w202_virtual_attrs,
};

static int w202_virtual_source(struct device *dev, unsigned int index,
			       struct thermal_zone_device **source)
{
	struct of_phandle_args args;
	struct platform_device *supplier;
	struct device_link *link;
	struct device_node *node;
	int ret;

	node = of_parse_phandle(dev->of_node, "thmzone-cells", index);
	if (!node)
		return -EINVAL;

	ret = of_parse_phandle_with_args(node, "thermal-sensors",
					 "#thermal-sensor-cells", 0, &args);
	if (ret)
		goto put_node;
	/* Reject direct recursion into our aggregate sensor. */
	if (args.np == dev->of_node) {
		ret = -EINVAL;
		of_node_put(args.np);
		goto put_node;
	}
	supplier = of_find_device_by_node(args.np);
	of_node_put(args.np);
	if (!supplier) {
		ret = -EPROBE_DEFER;
		goto put_node;
	}
	/* Managed links unbind this consumer before its sensor providers. */
	link = device_link_add(dev, &supplier->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&supplier->dev);
	if (!link) {
		ret = -EINVAL;
		goto put_node;
	}
	*source = thermal_zone_get_zone_by_name(node->name);
	ret = IS_ERR(*source) ? PTR_ERR(*source) : 0;
	if (ret == -ENODEV)
		ret = -EPROBE_DEFER;
put_node:
	of_node_put(node);
	return ret;
}

static int w202_virtual_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_virtual_thermal *data;
	struct thermal_zone_device *tz;
	int count, id, ret;
	unsigned int i;

	count = of_count_phandle_with_args(dev->of_node, "thmzone-cells", NULL);
	if (count <= 0 || count > 32)
		return dev_err_probe(dev, -EINVAL, "invalid source count %d\n", count);
	id = of_alias_get_id(dev->of_node, "thm-sensor");
	if (id < 0)
		return dev_err_probe(dev, id, "missing thm-sensor alias\n");
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->sources = devm_kcalloc(dev, count, sizeof(*data->sources), GFP_KERNEL);
	if (!data->sources)
		return -ENOMEM;
	data->count = count;
	for (i = 0; i < count; i++) {
		ret = w202_virtual_source(dev, i, &data->sources[i]);
		if (ret)
			return dev_err_probe(dev, ret, "source %u unavailable\n", i);
	}
	platform_set_drvdata(pdev, data);
	tz = devm_thermal_of_zone_register(dev, id, data, &w202_virtual_ops);
	if (IS_ERR(tz))
		return dev_err_probe(dev, PTR_ERR(tz), "aggregate zone registration failed\n");
	ret = devm_device_add_group(dev, &w202_virtual_group);
	if (ret)
		return ret;
	dev_info(dev, "registered maximum of %u sensors with stock DT trips/maps\n",
		 data->count);
	return 0;
}

static const struct of_device_id w202_virtual_match[] = {
	{ .compatible = "virtual-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, w202_virtual_match);

static struct platform_driver w202_virtual_driver = {
	.probe = w202_virtual_probe,
	.driver = {
		.name = "w202-virtual-thermal",
		.of_match_table = w202_virtual_match,
	},
};
module_platform_driver(w202_virtual_driver);

MODULE_DESCRIPTION("UMS9620 maximum-of-sensors virtual thermal zone");
MODULE_LICENSE("GPL");
