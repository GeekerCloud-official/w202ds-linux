// SPDX-License-Identifier: GPL-2.0-only
/* Read-only baseline. No DSI packets, resets, PHY test writes or DPU writes. */
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/regmap.h>

static int read_syscon(struct device_node *np, const char *name, u32 *raw,
		       u32 *mask)
{
	u32 args[2];
	struct regmap *map;
	int ret;

	map = syscon_regmap_lookup_by_phandle_args(np, name, 2, args);
	if (IS_ERR(map))
		return PTR_ERR(map);
	ret = regmap_read(map, args[0], raw);
	if (ret)
		return ret;
	*mask = args[1];
	pr_info("w202-snapshot: %s offset=%08x mask=%08x raw=%08x\n",
		name, args[0], *mask, *raw);
	return 0;
}

static int __init snapshot_init(void)
{
	static const u16 dpu_offsets[] = {
		0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
		0x20, 0x24, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40,
		0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60,
		0x230, 0x234, 0x238, 0x23c, 0x250, 0x258, 0x25c,
		0x260, 0x264, 0x268, 0x500, 0x1804, 0x18a0,
	};
	static const u16 dsi_offsets[] = {
		0x00, 0x04, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c,
		0x30, 0x34, 0x38, 0x40, 0x44, 0x48, 0x5c, 0x60,
		0x64, 0x68, 0x74, 0x78, 0x9c, 0xa0, 0xa4, 0xa8,
		0xac, 0xb0, 0xb4, 0xb8, 0xbc, 0xc0, 0xd0,
	};
	struct device_node *dsi, *dphy, *dpu, *pd;
	void __iomem *dsi_regs = NULL, *dpu_regs = NULL;
	struct clk *clock = NULL;
	u32 raw, mask;
	int i, ret;
	bool clock_on = false;

	dsi = of_find_node_by_path("/soc/dpuvsp/dsi@31300000");
	dphy = of_find_node_by_path("/soc/dpuvsp/dphy");
	dpu = of_find_node_by_path("/soc/dpuvsp/dpu@31000000");
	pd = of_find_node_by_path("/soc/mm/power-domain@0");
	if (!dsi || !dphy || !dpu || !pd ||
	    !of_device_is_compatible(dsi, "sprd,qogirn6pro-dsi-host")) {
		ret = -ENODEV;
		goto out;
	}
	ret = read_syscon(pd, "pmu-pwr-status-syscon", &raw, &mask);
	if (ret || (raw & mask)) {
		ret = ret ?: -EHOSTDOWN;
		goto out;
	}
	ret = read_syscon(dphy, "power-syscon", &raw, &mask);
	if (ret || (raw & mask)) {
		ret = ret ?: -EHOSTDOWN;
		goto out;
	}
	ret = read_syscon(dphy, "enable-syscon", &raw, &mask);
	if (ret || (raw & mask) != mask) {
		ret = ret ?: -EHOSTDOWN;
		goto out;
	}
	clock = of_clk_get_by_name(dsi, "clk_dsi0_eb");
	if (IS_ERR(clock)) {
		ret = PTR_ERR(clock);
		clock = NULL;
		goto out;
	}
	ret = clk_prepare_enable(clock);
	if (ret)
		goto out;
	clock_on = true;
	dsi_regs = of_iomap(dsi, 0);
	dpu_regs = of_iomap(dpu, 0);
	if (!dsi_regs || !dpu_regs) {
		ret = -ENOMEM;
		goto out;
	}
	if (readl(dpu_regs) != 0x600) {
		ret = -ENODEV;
		goto out;
	}
	pr_info("w202-snapshot: DPU version=%08x ctrl=%08x panel=%08x h=%08x v=%08x\n",
		readl(dpu_regs), readl(dpu_regs + 0x08),
		readl(dpu_regs + 0x18), readl(dpu_regs + 0x264),
		readl(dpu_regs + 0x268));
	for (i = 0; i < ARRAY_SIZE(dpu_offsets); i++)
		pr_info("w202-snapshot: DPU[%04x]=%08x\n", dpu_offsets[i],
			readl(dpu_regs + dpu_offsets[i]));
	for (i = 0; i < ARRAY_SIZE(dsi_offsets); i++)
		pr_info("w202-snapshot: DSI[%04x]=%08x\n", dsi_offsets[i],
			readl(dsi_regs + dsi_offsets[i]));
	for (i = 0; i < 23; i++)
		pr_info("w202-snapshot: DSC[%04x]=%08x\n", 0x1a00 + i * 4,
			readl(dpu_regs + 0x1a00 + i * 4));
	pr_info("w202-snapshot: COMPLETE\n");
	ret = 0;
out:
	if (dsi_regs)
		iounmap(dsi_regs);
	if (dpu_regs)
		iounmap(dpu_regs);
	if (clock_on)
		clk_disable_unprepare(clock);
	if (clock)
		clk_put(clock);
	of_node_put(dsi);
	of_node_put(dphy);
	of_node_put(dpu);
	of_node_put(pd);
	return ret;
}

static void __exit snapshot_exit(void) { }
module_init(snapshot_init);
module_exit(snapshot_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS read-only DSI/DSC baseline snapshot");
