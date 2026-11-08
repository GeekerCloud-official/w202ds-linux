/* SPDX-License-Identifier: GPL-2.0-only */
/* Built-in boot-display ownership. Native DRM uses the generic aperture API;
 * neither driver imports the other's symbols or knows its implementation.
 */
#include <linux/aperture.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/platform_device.h>

static_assert(IS_ENABLED(CONFIG_APERTURE_HELPERS));

static struct clk *pj4edk_clocks[6];
static unsigned int pj4edk_clocks_enabled;

static int pj4edk_display_probe(struct platform_device *pdev)
{
	static const char * const names[] = {
		"clk_dpuvsp_eb", "clk_dpuvsp_disp_eb", "clk_master_div6_eb",
		"clk_dpu_core", "clk_dpu_dpi", "clk_dsi0_eb",
	};
	struct device_node *dpu, *dsi;
	int i, ret;

	/* Bootloader-enabled hardware alone does not hold a Linux clock ref.
	 * VPU shares clk_dpuvsp_eb and may otherwise gate it on autosuspend,
	 * even with clk_ignore_unused. Acquire once, before userspace coldplug.
	 */
	dpu = of_find_node_by_path("/soc/dpuvsp/dpu@31000000");
	dsi = of_find_node_by_path("/soc/dpuvsp/dsi@31300000");
	if (!dpu || !dsi) {
		ret = -ENODEV;
		goto out_nodes;
	}
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		pj4edk_clocks[i] = of_clk_get_by_name(i == 5 ? dsi : dpu, names[i]);
		if (IS_ERR(pj4edk_clocks[i])) {
			ret = PTR_ERR(pj4edk_clocks[i]);
			pj4edk_clocks[i] = NULL;
			while (i)
				clk_put(pj4edk_clocks[--i]);
			goto out_nodes;
		}
	}
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ret = clk_prepare_enable(pj4edk_clocks[i]);
		if (ret)
			goto keep_scanout;
		pj4edk_clocks_enabled++;
	}
	ret = devm_aperture_acquire_for_platform_device(pdev, 0xb0000000, 0x9e4000);
	if (ret)
		goto keep_scanout;
	dev_info(&pdev->dev, "retained DPU/DSI clocks held until native aperture takeover\n");
	goto out_nodes;
keep_scanout:
	/* Never unwind successfully acquired boot clocks on a probe error:
	 * that could turn off the still-visible panel. Keep these references
	 * until reboot on this exceptional path; do not advertise success.
	 */
	dev_err(&pdev->dev, "clock ownership failed (%d); retaining %u boot clocks\n",
		ret, pj4edk_clocks_enabled);
out_nodes:
	of_node_put(dsi);
	of_node_put(dpu);
	return ret;
}

static void pj4edk_display_remove(struct platform_device *pdev)
{
	unsigned int i;

	/* The replacing driver must hold its own clocks before removal.
	 * The generic aperture callback synchronously quiesces our writer.
	 * No DPU stop, panel command, clock-rate or power-domain change here.
	 */
	pj4edk_boot_display_console_release();
	while (pj4edk_clocks_enabled)
		clk_disable_unprepare(pj4edk_clocks[--pj4edk_clocks_enabled]);
	for (i = 0; i < ARRAY_SIZE(pj4edk_clocks); i++) {
		clk_put(pj4edk_clocks[i]);
		pj4edk_clocks[i] = NULL;
	}
	if (inherited_dpu) {
		iounmap(inherited_dpu);
		inherited_dpu = NULL;
	}
}

static struct platform_driver pj4edk_display_driver = {
	.probe = pj4edk_display_probe,
	.remove = pj4edk_display_remove,
	.driver = {
		.name = "pj4edk-boot-display",
		.suppress_bind_attrs = true,
	},
};

static int __init pj4edk_display_init(void)
{
	struct platform_device *pdev;
	int ret;

	if (!pj4edk_boot_display_console_active())
		return 0;
	ret = platform_driver_register(&pj4edk_display_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("pj4edk-boot-display", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&pj4edk_display_driver);
		return PTR_ERR(pdev);
	}
	return 0;
}
late_initcall(pj4edk_display_init);
