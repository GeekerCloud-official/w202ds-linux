// SPDX-License-Identifier: GPL-2.0-only
/* W202DS fixed-panel DPU reset, clock and timing initialization.
 * Reference: Unisoc qogirn6pro/global_dpu.c and dpu/dpu_r6p0.c.
 * The shared VPU power domain remains on; this resets DPU and its VAU only.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <drm/drm_print.h>
#include "w202_r6p0.h"

static int w202_read_timing(struct w202_dpu *d, struct device_node *np,
                            struct drm_display_mode *m, u32 vfp)
{
 static const char * const props[] = {
  "clock-frequency", "hactive", "vactive", "hfront-porch",
  "hsync-len", "hback-porch", "vfront-porch", "vsync-len", "vback-porch",
 };
 u32 expected[] = {307200000, 1200, 1920, 156, 4, 160, vfp, 8, 28};
 u32 val[ARRAY_SIZE(props)];
 int i;

 if (!np)
  return -EINVAL;
 for (i = 0; i < ARRAY_SIZE(props); i++) {
  if (of_property_read_u32(np, props[i], &val[i]) || val[i] != expected[i])
   return dev_err_probe(d->dev, -EINVAL, "unsupported timing %s in %pOF\n",
                        props[i], np);
 }
 m->clock = val[0] / 1000;
 m->hdisplay = val[1];
 m->vdisplay = val[2];
 m->hsync_start = val[1] + val[3];
 m->hsync_end = m->hsync_start + val[4];
 m->htotal = m->hsync_end + val[5];
 m->vsync_start = val[2] + val[6];
 m->vsync_end = m->vsync_start + val[7];
 m->vtotal = m->vsync_end + val[8];
 m->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
 drm_mode_set_name(m);
 return 0;
}

int w202_dpu_read_mode(struct w202_dpu *d)
{
 struct device_node *panel, *timings, *native, *fast;
 int ret;

 panel = of_find_node_by_path("/lcds/lcd_guoxian_hx83102j_huaxing_nopen_mipi_himax");
 timings = of_get_child_by_name(panel, "display-timings");
 native = of_parse_phandle(timings, "native-mode", 0);
 fast = of_get_child_by_name(timings, "timing1");
 ret = w202_read_timing(d, native, &d->modes[0], 1408);
 if (!ret)
  ret = w202_read_timing(d, fast, &d->modes[1], 288);
 if (!ret)
  drm_mode_copy(&d->mode, &d->modes[0]);
 of_node_put(fast);
 of_node_put(native);
 of_node_put(timings);
 of_node_put(panel);
 return ret;
}

static int w202_reset(struct device_node *np, const char *property)
{
	struct regmap *map;
	u32 args[2];
	int ret;

	map = syscon_regmap_lookup_by_phandle_args(np, property, 2, args);
	if (IS_ERR(map))
		return PTR_ERR(map);
	ret = regmap_update_bits(map, args[0], args[1], args[1]);
	if (ret)
		return ret;
	udelay(10);
	return regmap_update_bits(map, args[0], args[1], 0);
}

static int w202_set_clock(struct device_node *np, struct clk *clock,
			  const char *source, unsigned long rate)
{
	struct clk *parent = of_clk_get_by_name(np, source);
	int ret;

	if (IS_ERR(parent))
		return PTR_ERR(parent);
	ret = clk_set_parent(clock, parent);
	clk_put(parent);
	if (ret)
		return ret;
	ret = clk_set_rate(clock, rate);
	if (ret)
		return ret;
	return clk_get_rate(clock) == rate ? 0 : -ERANGE;
}

int w202_dpu_hw_init(struct w202_dpu *d)
{
	struct device_node *np;
	const struct drm_display_mode *m = &d->mode;
	u32 size = m->vdisplay << 16 | m->hdisplay;
	u32 h = (m->hsync_start - m->hdisplay) << 20 |
		(m->htotal - m->hsync_end) << 8 | (m->hsync_end - m->hsync_start);
	u32 v = (m->vsync_start - m->vdisplay) << 20 |
		(m->vtotal - m->vsync_end) << 8 | (m->vsync_end - m->vsync_start);
	int i, ret;
	u32 before_size, reset_size, reset_layers;

	np = of_find_node_by_path("/soc/dpuvsp/dpu@31000000");
	if (!np)
		return -ENODEV;
	/* Caller has stopped scanout and disabled the backlight. */
	writel(0, d->regs + 0x250);
	ret = w202_set_clock(np, d->clks[3], "clk_src_614m4", 614400000);
	if (ret)
		goto out;
	ret = w202_set_clock(np, d->clks[4], "clk_src_307m2", m->clock * 1000UL);
	if (ret)
		goto out;
	before_size = readl(d->regs + 0x18);
	ret = w202_reset(np, "reset-syscon");
	if (ret)
		goto out;
	ret = w202_reset(np, "vau_reset-syscon");
	if (ret)
		goto out;
	if (readl(d->regs) != 0x600) {
		ret = -ENODEV;
		goto out;
	}
	reset_size = readl(d->regs + 0x18);
	reset_layers = readl(d->regs + 0x2c);
	drm_info(&d->drm, "DPU reset readback: panel before=%08x after=%08x layers=%08x\n",
		 before_size, reset_size, reset_layers);
	/* Single DSI video output, no scaling, enhancement or writeback. */
	writel(0, d->regs + 0x04);
	writel(0, d->regs + 0x0c);
	writel(0x00c4caca, d->regs + 0x10);
	writel(size, d->regs + 0x18);
	writel(size, d->regs + 0x1c);
	writel(0, d->regs + 0x20);
	writel(0, d->regs + 0x24);
	writel(0, d->regs + 0x2c);
	for (i = 0; i < 8; i++)
		writel(0, d->regs + 0x40 + i * 0x40);
	writel(0, d->regs + 0x234);
	writel(0, d->regs + 0x500);
	writel(BIT(16), d->regs + 0x260);
	writel(h, d->regs + 0x264);
	writel(v, d->regs + 0x268);
	/* Physical DMA scanout: translation is disabled, as on the baseline. */
	writel(8, d->regs + 0x1804);
	writel(0, d->regs + 0x18a0);
	writel(~0u, d->regs + 0x254);
	writel(d->irq_mask, d->regs + 0x250);
	if (readl(d->regs + 0x18) != size || readl(d->regs + 0x264) != h ||
	    readl(d->regs + 0x268) != v || readl(d->regs + 0x1804) != 8) {
		ret = -EIO;
		goto out;
	}
	d->hw_initialized = true;
	drm_info(&d->drm, "Linux DPU/VAU reset and init complete: core=%lu dpi=%lu h=%08x v=%08x\n",
		 clk_get_rate(d->clks[3]), clk_get_rate(d->clks[4]), h, v);
out:
	of_node_put(np);
	return ret;
}
