// SPDX-License-Identifier: GPL-2.0-only
/* W202DS UMS9620 platform support. Bounded CCF/devfreq core clock,
 * inherited voltage through the DT supply and full runtime domain cycling.
 * Real supply and hardware readbacks are mandatory before GPU-domain MMIO.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include "w202_gpu_qos.h"
#include "panfrost_ums9620.h"

/* A separate provider module owns the CCF callbacks. Registering these
 * clocks on Panfrost itself makes its two consumers pin their own module.
 * Panfrost's symbol dependency keeps this provider alive; clock consumers
 * also hold this provider, not the GPU driver. GPU devres unregisters the
 * clock before releasing its backing data on every unbind/probe failure.
 */
static struct device *ums_clock_provider;
static struct platform_device *ums_provider_pdev;
static int ums_provider_probe(struct platform_device *pdev)
{
 ums_clock_provider = &pdev->dev;
 return 0;
}
static struct platform_driver ums_provider_driver = {
 .probe = ums_provider_probe,
 .driver = { .name = "w202-gpu-support" },
};

struct ums_field { struct regmap *map; u32 reg, mask, old; };
struct ums_gpu {
	struct ums_field field[4];
	struct ums_field dvfs[7];
	struct clk_hw core_hw;
	struct device *dev;
	struct regmap *state_map, *aon;
	struct reset_control *rstc;
	struct mutex lock;
	void __iomem *nic;
	unsigned long rate;
	unsigned long suspends, resumes;
	bool accessible, domain_live;
	int voltage_uv;
	u32 state_reg, state_mask, off_state;
	struct clk *clk[16];
	struct regulator *supply;
	int n, enabled;
	int order[16];
	bool supply_enabled, modified, core_saved;
	bool dvfs_saved;
};

/* Only 153.6MHz and 384MHz are allowed core clock settings.
 * No GPLL changes or speculative high OPPs. Voltage stays at the measured
 * inherited value through the hardware voltage table as well as regulator.
 */
static unsigned int ums9620_freq_index = 3;
module_param(ums9620_freq_index, uint, 0444);
MODULE_PARM_DESC(ums9620_freq_index, "UMS9620 initial index: 2=153.6MHz, 3=384MHz; devfreq may change it");

static int ums_field_get(struct device_node *np, const char *name,
			 struct ums_field *f)
{
	u32 args[2];
	f->map = syscon_regmap_lookup_by_phandle_args(np, name, 2, args);
	if (IS_ERR(f->map)) return PTR_ERR(f->map);
	f->reg = args[0]; f->mask = args[1];
	return regmap_read(f->map, f->reg, &f->old);
}
static int ums_field_set(struct ums_field *f, u32 val)
{
	return regmap_update_bits(f->map, f->reg, f->mask, val);
}
static int ums_power_off(struct ums_gpu *g)
{
 u32 state;
 int i, ret = 0, err;

 /* No CCF source-clock calls while holding this lock: set_rate/recalc
  * take it under CCF's prepare lock. Close MMIO before disabling sources. */
 mutex_lock(&g->lock);
 g->accessible = false;
 if (g->domain_live) {
  if (g->dvfs_saved) {
   err = ums_field_set(&g->dvfs[5], g->dvfs[5].old);
   if (err && !ret) ret = err;
   udelay(100);
   err = ums_field_set(&g->dvfs[1], g->dvfs[1].old);
   if (err && !ret) ret = err;
   err = ums_field_set(&g->dvfs[0], g->dvfs[0].old);
   if (err && !ret) ret = err;
   for (i = 2; i < 7; i++) {
    if (i == 5) continue;
    err = ums_field_set(&g->dvfs[i], g->dvfs[i].old);
    if (err && !ret) ret = err;
   }
  }
  if (g->core_saved) {
   err = ums_field_set(&g->field[3], g->field[3].old);
   if (err && !ret) ret = err;
  }
 }
 if (ret) {
  mutex_unlock(&g->lock);
  return ret;
 }
 g->domain_live = false;
 mutex_unlock(&g->lock);
 while (g->enabled)
  clk_disable_unprepare(g->clk[g->order[--g->enabled]]);
 if (g->modified) {
  ret = ums_field_set(&g->field[2], g->field[2].old);
  if (ret) return ret;
  ret = regmap_read_poll_timeout(g->state_map, g->state_reg, state,
         (state & g->state_mask) == g->off_state, 50, 100000);
  if (ret) {
   dev_err(g->dev, "GPU shutdown state=%08x expected=%08x error=%d\n",
           state, g->off_state, ret);
   /* Keep supply/PLL owned until shutdown is confirmed. */
   return ret;
  }
  udelay(50);
  ret = ums_field_set(&g->field[1], g->field[1].old);
  if (ret) return ret;
  ret = ums_field_set(&g->field[0], g->field[0].old);
  if (ret) return ret;
  g->modified = false;
 }
 if (g->supply_enabled) {
  ret = regulator_disable(g->supply);
  if (ret) return ret;
  g->supply_enabled = false;
 }
 return 0;
}

static void ums_cleanup(void *data)
{
 struct ums_gpu *g = data;
 int i, ret = ums_power_off(g);
 if (ret)
  dev_err(g->dev, "GPU cleanup power-off failed: %d\n", ret);
 for (i = 0; i < g->n; i++)
  if (g->clk[i]) clk_put(g->clk[i]);
 if (g->supply) regulator_put(g->supply);
}

static int ums_frequency_init(struct ums_gpu *g, struct device *dev,
			       struct device_node *np, int uv)
{
	struct regmap *map;
	u32 value, index = g->rate == 153600000 ? 2 : 3;
	int i, ret;

	if (ums9620_freq_index != 2 && ums9620_freq_index != 3)
		return -EINVAL;
	/* Both OPPs are exposed even when the initial selector is 2. */
	if (uv < 650000)
		return -ERANGE;
	if (uv % 3125)
		return -ERANGE;
	if (!g->dvfs_saved) {
	ret = ums_field_get(np, "top_dvfs_cfg", &g->dvfs[0]);
	if (ret) return ret;
	ret = ums_field_get(np, "gpu_sw_dvfs_ctrl", &g->dvfs[1]);
	if (ret) return ret;
	ret = ums_field_get(np, "dcdc_gpu_voltage0", &g->dvfs[2]);
	if (ret) return ret;
	map = syscon_regmap_lookup_by_phandle(np, "sprd,gpu-dvfs-apb-syscon");
	if (IS_ERR(map)) return PTR_ERR(map);
	g->dvfs[3] = (struct ums_field){map, 0x160, 0x3c0};
	g->dvfs[4] = (struct ums_field){map, 0x9ac, 3};
	g->dvfs[5] = (struct ums_field){map, 0x794, 7};
	g->dvfs[6] = (struct ums_field){map, 0x15c, 0x3c0};
	for (i = 3; i < 7; i++) {
		ret = regmap_read(map, g->dvfs[i].reg, &g->dvfs[i].old);
		if (ret) return ret;
	}
	/* Stock maps use selector 2=153.6 / 3=384 with no divider. */
	if ((g->dvfs[3].old & 0x3f) != 3 ||
	    (g->dvfs[6].old & 0x3f) != 2)
		return -EINVAL;
	g->dvfs_saved = true;
	}
	map = g->dvfs[5].map;
	/* A top-domain reset may restore hardware defaults; validate source
	 * selectors again before making a frequency index usable. */
	ret = regmap_read(map, 0x160, &value);
	if (ret) return ret;
	if ((value & 0x3f) != 3) return -EIO;
	ret = regmap_read(map, 0x15c, &value);
	if (ret) return ret;
	if ((value & 0x3f) != 2) return -EIO;
	/* Slot 0 holds precisely the existing supply voltage. Point the
	 * chosen frequency at that slot, keeping voltage unchanged when
	 * hardware DVFS takes control. The vendor uses 3.125mV selectors.
	 */
	ret = ums_field_set(&g->dvfs[2], uv / 3125);
	if (ret) return ret;
	ret = ums_field_set(&g->dvfs[3], 0);
	if (ret) return ret;
	ret = ums_field_set(&g->dvfs[6], 0);
	if (ret) return ret;
	ret = ums_field_set(&g->dvfs[4], 1);
	if (ret) return ret;
	ret = ums_field_set(&g->dvfs[0], 0);
	if (ret) return ret;
	ret = ums_field_set(&g->dvfs[1], 0);
	if (ret) return ret;
	ret = ums_field_set(&g->dvfs[5], index);
	if (ret) return ret;
	usleep_range(1000, 1500);
	ret = regmap_read(map, 0x794, &value);
	if (ret) return ret;
	if ((value & 7) != index)
		return -EIO;
	dev_dbg(dev, "bounded GPU HW index=%u target=%uHz voltage-slot0=%duV; confirm with cycle counter\n",
		index, index == 2 ? 153600000 : 384000000, uv);
	return 0;
}

/* CCF queries and governor changes can occur with the top domain off.
 * Report/store the configured rate then; never touch GPU MMIO while off. */
static unsigned long ums_core_recalc_rate(struct clk_hw *hw,
                                         unsigned long parent_rate)
{
 struct ums_gpu *g = container_of(hw, struct ums_gpu, core_hw);
 unsigned long rate;
 u32 index, map;

 mutex_lock(&g->lock);
 rate = g->rate;
 if (g->accessible) {
  if (regmap_read(g->dvfs[5].map, 0x794, &index)) {
   rate = 0;
   goto out;
  }
  index &= 7;
  if ((index != 2 && index != 3) ||
      regmap_read(g->dvfs[5].map, 0x154 + 4 * index, &map) ||
      (map & 0x3ff) != index) {
   rate = 0;
   goto out;
  }
  rate = index == 2 ? 153600000 : 384000000;
 }
out:
 mutex_unlock(&g->lock);
 return rate;
}

static long ums_core_round_rate(struct clk_hw *hw, unsigned long rate,
                               unsigned long *parent_rate)
{
 return rate <= 268800000 ? 153600000 : 384000000;
}

static int ums_core_set_rate(struct clk_hw *hw, unsigned long rate,
                            unsigned long parent_rate)
{
 struct ums_gpu *g = container_of(hw, struct ums_gpu, core_hw);
 u32 index, value;
 int ret = 0;

 if (rate != 153600000 && rate != 384000000)
  return -EINVAL;
 index = rate == 153600000 ? 2 : 3;
 mutex_lock(&g->lock);
 if (g->accessible) {
  ret = regmap_read(g->dvfs[5].map, 0x154 + 4 * index, &value);
  if (ret) goto out;
  if ((value & 0x3ff) != index) {
   ret = -EIO;
   goto out;
  }
  ret = ums_field_set(&g->dvfs[5], index);
  if (ret) goto out;
  usleep_range(1000, 1500);
  ret = regmap_read(g->dvfs[5].map, 0x794, &value);
  if (ret) goto out;
  if ((value & 7) != index) {
   ret = -EIO;
   goto out;
  }
 }
 g->rate = rate;
out:
 mutex_unlock(&g->lock);
 return ret;
}

static const struct clk_ops ums_core_ops = {
 .recalc_rate = ums_core_recalc_rate,
 .round_rate = ums_core_round_rate,
 .set_rate = ums_core_set_rate,
};

static void ums_unregister_core_clock(void *data)
{
 clk_hw_unregister(data);
}

static int ums_register_core_clock(struct ums_gpu *g,
                                   struct panfrost_device *pfdev)
{
 struct device *dev = pfdev->dev;
 struct clk_init_data init = {
  .ops = &ums_core_ops,
  .flags = CLK_GET_RATE_NOCACHE,
 };
 int ret;

 init.name = devm_kasprintf(dev, GFP_KERNEL, "%s-core", dev_name(dev));
 if (!init.name)
  return -ENOMEM;
 g->core_hw.init = &init;
 if (!ums_clock_provider)
  return -EPROBE_DEFER;
 ret = clk_hw_register(ums_clock_provider, &g->core_hw);
 if (ret)
  return ret;
 ret = devm_add_action_or_reset(dev, ums_unregister_core_clock, &g->core_hw);
 if (ret)
  return ret;
 ret = devm_clk_hw_register_clkdev(dev, &g->core_hw, "gpu-core", dev_name(dev));
 if (ret)
  return ret;
 pfdev->clock = devm_clk_get(dev, "gpu-core");
 if (IS_ERR(pfdev->clock))
  return PTR_ERR(pfdev->clock);
 dev_info(dev, "core clock registered: %luHz, bounded 153.6/384MHz devfreq\n",
          clk_get_rate(pfdev->clock));
 return 0;
}

static int ums_init_bus(struct ums_gpu *g, struct device_node *np)
{
 struct regmap *apb;
 void __iomem *nic;
 u32 value;
 unsigned int i, port;
 int ret;

 apb = syscon_regmap_lookup_by_phandle(np, "sprd,gpu-apb-syscon");
 if (IS_ERR(apb)) return PTR_ERR(apb);
 if (!g->nic) {
  g->nic = devm_ioremap(g->dev, 0x23018000, 0x100);
  if (!g->nic) return -ENOMEM;
 }
 nic = g->nic;
 for (port = 0; port < 2; port++) {
  for (i = 0; i < ARRAY_SIZE(ums_nic_qos); i++) {
   const struct ums_qos_reg *q = &ums_nic_qos[i];
   void __iomem *reg = nic + port * 0x80 + q->offset;
   value = (readl(reg) & ~q->mask) | q->value;
   writel(value, reg);
   if ((readl(reg) & q->mask) != q->value) return -EIO;
  }
 }
 /* Re-arm the vendor's per-port low-power handshake after every full
  * GPU-system reset. Do not disable runtime power management globally. */
 for (port = 0; port < 2; port++) {
  ret = regmap_update_bits(apb, 4 + port * 4, BIT(16), 0);
  if (ret) return ret;
  ret = regmap_update_bits(apb, 4 + port * 4, BIT(16), BIT(16));
  if (ret) return ret;
  ret = regmap_read(apb, 4 + port * 4, &value);
  if (ret) return ret;
  if (!(value & BIT(16))) return -EIO;
 }
 udelay(200);
 dev_dbg(g->dev, "GPU NIC400 QoS and LPC handshakes initialized\n");
 return 0;
}

static int ums_power_on(struct ums_gpu *g)
{
 struct device *dev = g->dev;
 struct device_node *np = dev->of_node;
 u32 state, gate, reset;
 int ret, uv;

 if (READ_ONCE(g->accessible)) return 0;
	if (!g->supply_enabled) {
		ret = regulator_enable(g->supply);
		if (ret) return ret;
		g->supply_enabled = true;
	}
	usleep_range(1000, 1500);
	if (regulator_is_enabled(g->supply) != 1) return -EHOSTDOWN;
	uv = regulator_get_voltage(g->supply);
	if (uv != g->voltage_uv || uv < 650000 || uv > 750000)
		return -ERANGE;
	g->modified = true;
	ret = ums_field_set(&g->field[0], 0); if (ret) return ret;
	udelay(100);
	ret = ums_field_set(&g->field[1], g->field[1].mask); if (ret) return ret;
	udelay(150);
	ret = ums_field_set(&g->field[2], 0); if (ret) return ret;
	ret = regmap_read_poll_timeout(g->state_map, g->state_reg, state,
				     !(state & g->state_mask), 100, 100000);
	if (ret) return ret;
	while (g->enabled < g->n) {
		int i = g->enabled + 1;
		int index = i == g->n ? 0 : i;
		ret = clk_prepare_enable(g->clk[index]); if (ret) return ret;
		g->order[g->enabled++] = index;
	}
	/* GPU-system reset must see live interface/source clocks. LK left
	 * the interface gate enabled on first boot; after an unbind it is
	 * disabled. A pulse before clk_prepare_enable can leave stale GPU
	 * state on the next probe even though GPU_ID and RESET_COMPLETED work.
	 */
	ret = reset_control_assert(g->rstc); if (ret) return ret;
	udelay(10);
	ret = reset_control_deassert(g->rstc); if (ret) return ret;
	udelay(300);
	ret = regmap_read(g->aon, 0, &gate); if (ret) return ret;
	ret = regmap_read(g->field[2].map, 0xb98, &reset); if (ret) return ret;
	if (!(gate & BIT(11)) || (reset & BIT(2))) return -EHOSTDOWN;
	dev_dbg(dev, "GPU supply=%duV gate=%#x reset=%#x top=%#x; accessing APB\n", uv, gate, reset, state);
	mutex_lock(&g->lock);
	g->domain_live = true;
	if (!g->core_saved) {
		ret = ums_field_get(np, "clk_core_gpu_eb", &g->field[3]);
		if (ret) goto out_unlock;
		g->core_saved = true;
	}
	ret = ums_field_set(&g->field[3], g->field[3].mask); if (ret) goto out_unlock;
	udelay(400);
	ret = ums_frequency_init(g, dev, np, uv);
	if (ret) goto out_unlock;
	ret = ums_init_bus(g, np);
	if (ret) goto out_unlock;
	g->accessible = true;
out_unlock:
	mutex_unlock(&g->lock);
	return ret;
}

int panfrost_ums9620_runtime_suspend(struct panfrost_device *pfdev)
{
 struct ums_gpu *g = pfdev->platform_data;
 int ret = ums_power_off(g);
 if (!ret) g->suspends++;
 return ret;
}
EXPORT_SYMBOL_GPL(panfrost_ums9620_runtime_suspend);

int panfrost_ums9620_runtime_resume(struct panfrost_device *pfdev)
{
 struct ums_gpu *g = pfdev->platform_data;
 int ret = ums_power_on(g);
 if (!ret) g->resumes++;
 else ums_power_off(g);
 return ret;
}
EXPORT_SYMBOL_GPL(panfrost_ums9620_runtime_resume);

static ssize_t w202_gpu_power_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
 struct panfrost_device *pfdev = dev_get_drvdata(dev);
 struct ums_gpu *g = pfdev->platform_data;
 u32 top, gate;
 int ret;
 ssize_t len;

 mutex_lock(&g->lock);
 /* Only always-on registers: this read must not resume the GPU. */
 ret = regmap_read(g->state_map, g->state_reg, &top);
 if (!ret) ret = regmap_read(g->aon, 0, &gate);
 if (ret) len = ret;
 else len = sysfs_emit(buf,
  "accessible=%u top=%08x interface=%u supply_owned=%u configured_hz=%lu suspends=%lu resumes=%lu\n",
  g->accessible, top & g->state_mask, !!(gate & BIT(11)),
  g->supply_enabled, g->rate, g->suspends, g->resumes);
 mutex_unlock(&g->lock);
 return len;
}
static DEVICE_ATTR_RO(w202_gpu_power);
static struct attribute *ums_attrs[] = { &dev_attr_w202_gpu_power.attr, NULL };
static const struct attribute_group ums_group = { .attrs = ums_attrs };

int panfrost_ums9620_init(struct panfrost_device *pfdev)
{
	static const char * const names[] = {
		"gpll_cfg_frc_off", "gpll_cfg_frc_on", "top_force_shutdown",
	};
	struct device *dev = pfdev->dev;
	struct device_node *np = dev->of_node, *clock_node;
	struct regmap *aon;
	struct ums_gpu *g;
	u32 state_args[2], state;
	struct regmap *state_map;
	int i, ret, uv;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g) return -ENOMEM;
	g->dev = dev;
	mutex_init(&g->lock);
	g->rate = ums9620_freq_index == 2 ? 153600000 : 384000000;
	pfdev->platform_data = g;
	ret = devm_add_action_or_reset(dev, ums_cleanup, g);
	if (ret) return ret;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		ret = ums_field_get(np, names[i], &g->field[i]);
		if (ret) return ret;
	}
	/* Do not interfere with another owner of an already powered domain. */
	if (!(g->field[2].old & g->field[2].mask)) return -EBUSY;
	g->supply = regulator_get_optional(dev, "gpu");
	if (IS_ERR(g->supply)) {
		ret = PTR_ERR(g->supply); g->supply = NULL; return ret;
	}
	uv = regulator_get_voltage(g->supply);
	g->voltage_uv = uv;
	/* Stock GPU OPPs span 550-750mV. LK may leave either 650mV or
	 * 750mV across boots. Preserve it; do not program a voltage selector. */
	if (uv < 550000 || uv > 750000)
		return dev_err_probe(dev, -ERANGE, "inherited supply outside stock GPU OPP range: %d\n", uv);
	g->n = of_clk_get_parent_count(np);
	if (g->n < 2 || g->n > ARRAY_SIZE(g->clk)) { g->n = 0; return -EINVAL; }
	for (i = 0; i < g->n; i++) {
		g->clk[i] = of_clk_get(np, i);
		if (IS_ERR(g->clk[i])) {
			ret = PTR_ERR(g->clk[i]); g->clk[i] = NULL; return ret;
		}
	}
	state_map = syscon_regmap_lookup_by_phandle_args(np, "gpu_top_state", 2, state_args);
	if (IS_ERR(state_map)) return PTR_ERR(state_map);
	g->state_map = state_map;
	g->state_reg = state_args[0];
	g->state_mask = state_args[1];
	ret = regmap_read(state_map, state_args[0], &state);
	if (ret) return ret;
	g->off_state = state & state_args[1];
	if (!g->off_state) return -EBUSY;
	clock_node = of_parse_phandle(np, "clocks", 0);
	if (!clock_node) return -ENODEV;
	aon = syscon_regmap_lookup_by_phandle(clock_node, "sprd,syscon");
	of_node_put(clock_node);
	if (IS_ERR(aon)) return PTR_ERR(aon);
	g->aon = aon;
	g->rstc = pfdev->rstc;
	ret = ums_power_on(g);
	if (ret) return ret;
	ret = ums_register_core_clock(g, pfdev);
	if (ret) return ret;
	pfdev->bus_clock = g->clk[0];
	return devm_device_add_group(dev, &ums_group);
}

EXPORT_SYMBOL_GPL(panfrost_ums9620_init);
static int __init ums_support_init(void)
{
 int ret = platform_driver_register(&ums_provider_driver);
 if (ret) return ret;
 ums_provider_pdev = platform_device_register_simple("w202-gpu-support", -1, NULL, 0);
 if (IS_ERR(ums_provider_pdev)) {
  ret = PTR_ERR(ums_provider_pdev);
  platform_driver_unregister(&ums_provider_driver);
  return ret;
 }
 return 0;
}
static void __exit ums_support_exit(void)
{
 platform_device_unregister(ums_provider_pdev);
 ums_clock_provider = NULL;
 platform_driver_unregister(&ums_provider_driver);
}
module_init(ums_support_init);
module_exit(ums_support_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202 UMS9620 GPU power, NIC400 and CCF provider");
