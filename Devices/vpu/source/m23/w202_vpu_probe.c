// SPDX-License-Identifier: GPL-2.0-only
/* Read-only VAU snapshot with proper VPU domain and clock references.
 * No reset, translation, job or shared-QoS register writes are performed.
 */
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include "w202_vpu.h"

static unsigned int core;
module_param(core, uint, 0444);
MODULE_PARM_DESC(core, "Core 0=encoder0, 1=encoder1, 2=decoder; fixed at insertion");
static unsigned int registers[12];
static unsigned int register_count = ARRAY_SIZE(registers);
module_param_array_named(snapshot, registers, uint, &register_count, 0444);
static unsigned long rates[5];
static unsigned int rate_count = ARRAY_SIZE(rates);
module_param_array_named(clock_rates, rates, ulong, &rate_count, 0444);

static const char * const paths[] = {
	"/soc/mm/video-codec@32000000",
	"/soc/mm/enc-core1@32100000",
	"/soc/mm/video-codec@32200000",
};
static const char * const compatibles[] = {
	"sprd,vpu-enc-core0", "sprd,vpu-enc-core1", "sprd,vpu-dec-core0",
};
static const char * const clock_names[] = {
	"clk_domain_eb", "clk_dev_eb", "clk_ahb_vsp", "clk_vsp", "clk_ckg_eb",
};
static const unsigned int offsets[] = {
	0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0xa0, 0xa8, 0xac,
};
static struct platform_device *client;
static struct device *power_device;
static struct clk_bulk_data clocks[ARRAY_SIZE(clock_names)];
static unsigned int clocks_acquired;
static bool clocks_enabled;
static bool power_active;
static void __iomem *base;

static void probe_cleanup(void)
{
	if (base) {
		iounmap(base);
		base = NULL;
	}
	if (clocks_enabled) {
		clk_bulk_disable_unprepare(clocks_acquired, clocks);
		clocks_enabled = false;
	}
	while (clocks_acquired)
		clk_put(clocks[--clocks_acquired].clk);
	if (power_device) {
		if (power_active)
			pm_runtime_put_sync_suspend(power_device);
		dev_pm_domain_detach(power_device, true);
		power_device = NULL;
		power_active = false;
	}
	if (client) {
		platform_device_put(client);
		client = NULL;
	}
}

static int __init probe_init(void)
{
	struct device_node *node;
	struct resource resource;
	u32 version;
	int i, ret;

	if (core >= ARRAY_SIZE(paths) || !w202_vpu_power_ready())
		return -EINVAL;
	node = of_find_node_by_path(paths[core]);
	if (!node || !of_device_is_compatible(node, compatibles[core]) ||
	    !of_device_is_available(node)) {
		of_node_put(node);
		return -ENODEV;
	}
	client = of_find_device_by_node(node);
	of_node_put(node);
	if (!client)
		return -ENODEV;
	device_lock(&client->dev);
	if (client->dev.driver) {
		ret = -EBUSY;
		goto fail;
	}
	ret = of_property_read_u32(client->dev.of_node, "sprd,video_ip_version", &version);
	if (ret || version != 0x15) {
		ret = -ENODEV;
		goto fail;
	}
	ret = of_address_to_resource(client->dev.of_node, 0, &resource);
	if (ret || resource.start != 0x32000000 + core * 0x100000 ||
	    resource_size(&resource) != 0xc000) {
		ret = -EINVAL;
		goto fail;
	}
	power_device = dev_pm_domain_attach_by_id(&client->dev, 0);
	if (IS_ERR(power_device)) {
		ret = PTR_ERR(power_device);
		power_device = NULL;
		goto fail;
	}
	if (!power_device) {
		ret = -ENODEV;
		goto fail;
	}
	ret = pm_runtime_resume_and_get(power_device);
	if (ret < 0)
		goto fail;
	power_active = true;
	for (i = 0; i < ARRAY_SIZE(clock_names); i++) {
		clocks[i].id = clock_names[i];
		clocks[i].clk = of_clk_get_by_name(client->dev.of_node, clock_names[i]);
		if (IS_ERR(clocks[i].clk)) {
			ret = PTR_ERR(clocks[i].clk);
			goto fail;
		}
		clocks_acquired++;
	}
	ret = clk_bulk_prepare_enable(clocks_acquired, clocks);
	if (ret)
		goto fail;
	clocks_enabled = true;
	base = ioremap(resource.start, 0x100);
	if (!base) {
		ret = -ENOMEM;
		goto fail;
	}
	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		registers[i] = readl(base + offsets[i]);
	for (i = 0; i < ARRAY_SIZE(rates); i++)
		rates[i] = clk_get_rate(clocks[i].clk);
	device_unlock(&client->dev);
	pr_info("w202-vpu: core%u read-only VAU snapshot version=%08x cfg=%08x\n",
		core, registers[0], registers[1]);
	return 0;
fail:
	device_unlock(&client->dev);
	probe_cleanup();
	return ret;
}

module_init(probe_init);
module_exit(probe_cleanup);
MODULE_DESCRIPTION("W202DS VPU power/clock/read-only VAU bring-up probe");
MODULE_LICENSE("GPL");
MODULE_VERSION("w202ds-vpu-probe-v1");
