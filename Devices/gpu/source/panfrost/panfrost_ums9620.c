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
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>
#include "w202_gpu_qos.h"
#include "panfrost_ums9620.h"
#include "panfrost_ums9620_thermal.h"

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

/* GPU DVFS indices 2..5 select 153.6, 384 and 512MHz (TGPLL) and GPLL/2.5;
 * the stock index maps carry selector == index. Every index used here points
 * at voltage slot 0, which holds the measured inherited supply: a frequency
 * change never moves the voltage. 512MHz and GPLL/2.5 = 655.2MHz (GPLL at
 * 1638MHz, the stock T760 maximum) are offered only on a T760 whose GPU bin
 * the stock driver runs at those rates with no more than the inherited
 * supply. T770/T790 rates (680, 780, 850MHz) are never used.
 */
#define UMS_IDX_LO 2
#define UMS_IDX_HI 5
#define UMS_MAP(i) (0x154 + 4 * (i))
#define UMS_T760_GPLL 1638000000UL

struct ums_gpu {
	struct ums_field field[4];
	struct ums_field top_dvfs, sw_dvfs, volt0, dvfs_9ac, dvfs_index;
	struct ums_field map[UMS_IDX_HI + 1];
	unsigned long rates[UMS_IDX_HI + 1];	/* by index; 0 = not offered */
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
	struct dev_pm_qos_request thermal_qos;
	struct delayed_work thermal_work;
	struct thermal_zone_device *tz;
	bool hot;
	bool sensor_fault;
};

static unsigned int ums9620_freq_index = 3;
module_param(ums9620_freq_index, uint, 0444);
MODULE_PARM_DESC(ums9620_freq_index, "UMS9620 initial index: 2=153.6MHz, 3=384MHz; devfreq may change it");

static int ums_rate_index(struct ums_gpu *g, unsigned long rate)
{
	int i;

	for (i = UMS_IDX_LO; i <= UMS_IDX_HI; i++)
		if (g->rates[i] && g->rates[i] == rate)
			return i;
	return -EINVAL;
}

static unsigned long ums_max_rate(struct ums_gpu *g)
{
	int i;

	for (i = UMS_IDX_HI; i > UMS_IDX_LO; i--)
		if (g->rates[i])
			break;
	return g->rates[i];
}

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
   err = ums_field_set(&g->dvfs_index, g->dvfs_index.old);
   if (err && !ret) ret = err;
   udelay(100);
   err = ums_field_set(&g->sw_dvfs, g->sw_dvfs.old);
   if (err && !ret) ret = err;
   err = ums_field_set(&g->top_dvfs, g->top_dvfs.old);
   if (err && !ret) ret = err;
   err = ums_field_set(&g->volt0, g->volt0.old);
   if (err && !ret) ret = err;
   for (i = UMS_IDX_LO; i <= UMS_IDX_HI; i++) {
    err = ums_field_set(&g->map[i], g->map[i].old);
    if (err && !ret) ret = err;
   }
   err = ums_field_set(&g->dvfs_9ac, g->dvfs_9ac.old);
   if (err && !ret) ret = err;
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
	u32 value;
	int i, ret, index = ums_rate_index(g, g->rate);

	if (index < 0)
		return index;
	if (uv < 650000)
		return -ERANGE;
	if (uv % 3125)
		return -ERANGE;
	if (!g->dvfs_saved) {
	ret = ums_field_get(np, "top_dvfs_cfg", &g->top_dvfs);
	if (ret) return ret;
	ret = ums_field_get(np, "gpu_sw_dvfs_ctrl", &g->sw_dvfs);
	if (ret) return ret;
	ret = ums_field_get(np, "dcdc_gpu_voltage0", &g->volt0);
	if (ret) return ret;
	map = syscon_regmap_lookup_by_phandle(np, "sprd,gpu-dvfs-apb-syscon");
	if (IS_ERR(map)) return PTR_ERR(map);
	g->dvfs_9ac = (struct ums_field){map, 0x9ac, 3};
	g->dvfs_index = (struct ums_field){map, 0x794, 7};
	ret = regmap_read(map, g->dvfs_9ac.reg, &g->dvfs_9ac.old);
	if (ret) return ret;
	ret = regmap_read(map, g->dvfs_index.reg, &g->dvfs_index.old);
	if (ret) return ret;
	for (i = UMS_IDX_LO; i <= UMS_IDX_HI; i++) {
		g->map[i] = (struct ums_field){map, UMS_MAP(i), 0x3c0};
		ret = regmap_read(map, g->map[i].reg, &g->map[i].old);
		if (ret) return ret;
	}
	/* Stock maps use selector 2=153.6 / 3=384 with no divider. */
	if ((g->map[3].old & 0x3f) != 3 ||
	    (g->map[2].old & 0x3f) != 2)
		return -EINVAL;
	g->dvfs_saved = true;
	}
	map = g->dvfs_index.map;
	/* A top-domain reset may restore hardware defaults; validate source
	 * selectors again before making a frequency index usable. */
	for (i = UMS_IDX_LO; i <= UMS_IDX_HI; i++) {
		ret = regmap_read(map, UMS_MAP(i), &value);
		if (ret) return ret;
		if ((value & 0x3f) != i && g->rates[i]) return -EIO;
	}
	/* Slot 0 holds precisely the existing supply voltage. Point every
	 * index used at that slot, keeping voltage unchanged when hardware
	 * DVFS takes control. The vendor uses 3.125mV selectors.
	 */
	ret = ums_field_set(&g->volt0, uv / 3125);
	if (ret) return ret;
	for (i = UMS_IDX_LO; i <= UMS_IDX_HI; i++) {
		ret = ums_field_set(&g->map[i], 0);
		if (ret) return ret;
	}
	ret = ums_field_set(&g->dvfs_9ac, 1);
	if (ret) return ret;
	ret = ums_field_set(&g->top_dvfs, 0);
	if (ret) return ret;
	ret = ums_field_set(&g->sw_dvfs, 0);
	if (ret) return ret;
	ret = ums_field_set(&g->dvfs_index, index);
	if (ret) return ret;
	usleep_range(1000, 1500);
	ret = regmap_read(map, g->dvfs_index.reg, &value);
	if (ret) return ret;
	if ((value & 7) != index)
		return -EIO;
	dev_dbg(dev, "bounded GPU HW index=%d target=%luHz voltage-slot0=%duV\n",
		index, g->rates[index], uv);
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
  if (regmap_read(g->dvfs_index.map, g->dvfs_index.reg, &index)) {
   rate = 0;
   goto out;
  }
  index &= 7;
  if (index < UMS_IDX_LO || index > UMS_IDX_HI || !g->rates[index] ||
      regmap_read(g->dvfs_index.map, UMS_MAP(index), &map) ||
      (map & 0x3ff) != index) {
   rate = 0;
   goto out;
  }
  rate = g->rates[index];
 }
out:
 mutex_unlock(&g->lock);
 return rate;
}

/* Offered rates ascend with the index; take the fastest not above rate. */
static long ums_core_round_rate(struct clk_hw *hw, unsigned long rate,
                               unsigned long *parent_rate)
{
 struct ums_gpu *g = container_of(hw, struct ums_gpu, core_hw);
 unsigned long best = g->rates[UMS_IDX_LO];
 int i;

 for (i = UMS_IDX_LO; i <= UMS_IDX_HI; i++)
  if (g->rates[i] && g->rates[i] <= rate)
   best = g->rates[i];
 return best;
}

static int ums_core_set_rate(struct clk_hw *hw, unsigned long rate,
                            unsigned long parent_rate)
{
 struct ums_gpu *g = container_of(hw, struct ums_gpu, core_hw);
 int index = ums_rate_index(g, rate);
 u32 value;
 int ret = 0;

 if (index < 0)
  return index;
 mutex_lock(&g->lock);
 if (g->accessible) {
  ret = regmap_read(g->dvfs_index.map, UMS_MAP(index), &value);
  if (ret) goto out;
  if ((value & 0x3ff) != index) {
   ret = -EIO;
   goto out;
  }
  ret = ums_field_set(&g->dvfs_index, index);
  if (ret) goto out;
  usleep_range(1000, 1500);
  ret = regmap_read(g->dvfs_index.map, g->dvfs_index.reg, &value);
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
 dev_info(dev, "core clock registered: %luHz, devfreq %lu-%luHz\n",
          clk_get_rate(pfdev->clock), g->rates[UMS_IDX_LO], ums_max_rate(g));
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
  "accessible=%u top=%08x interface=%u supply_owned=%u configured_hz=%lu max_hz=%lu thermal_cap=%u suspends=%lu resumes=%lu\n",
  g->accessible, top & g->state_mask, !!(gate & BIT(11)),
  g->supply_enabled, g->rate, ums_max_rate(g), READ_ONCE(g->hot),
  g->suspends, g->resumes);
 mutex_unlock(&g->lock);
 return len;
}
static DEVICE_ATTR_RO(w202_gpu_power);
static struct attribute *ums_attrs[] = { &dev_attr_w202_gpu_power.attr, NULL };
static const struct attribute_group ums_group = { .attrs = ums_attrs };

/* Stock T760 GPU voltage for 512MHz and 655.2MHz by efuse gpu_bin
 * (1 FF, 2 TT, 3 SS; the vendor treats 0 as SS).
 */
static const int ums_t760_uv[4][2] = {
	{ 700000, 750000 }, { 650000, 650000 },
	{ 650000, 700000 }, { 700000, 750000 },
};

static int ums_t760_rates(struct ums_gpu *g)
{
	struct device_node *hwf = of_find_node_by_path("/hwfeature/auto");
	const char *soc = NULL;
	struct clk *gpll = NULL;
	bool t760;
	u32 bin = 0;
	int i, ret;

	if (hwf)
		of_property_read_string(hwf, "efuse", &soc);
	t760 = soc && !strcmp(soc, "T760");
	of_node_put(hwf);
	if (!t760) {
		dev_info(g->dev, "GPU efuse type unknown: 153.6/384MHz only\n");
		return 0;
	}
	ret = nvmem_cell_read_variable_le_u32(g->dev, "gpu_bin", &bin);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret || bin > 3) {
		dev_warn(g->dev, "GPU bin unreadable (%d, %u): rated as SS\n", ret, bin);
		bin = 0;
	}
	if (g->voltage_uv >= ums_t760_uv[bin][0] && (g->map[4].old & 0x3f) == 4)
		g->rates[4] = 512000000;
	if (g->rates[4] && g->voltage_uv >= ums_t760_uv[bin][1] &&
	    (g->map[5].old & 0x3f) == 5) {
		for (i = 0; i < g->n; i++)
			if (!strcmp(__clk_get_name(g->clk[i]), "gpll"))
				gpll = g->clk[i];
		/* The GPU runs from TGPLL (index 2/3) here: GPLL is idle. */
		ret = gpll ? clk_set_rate(gpll, UMS_T760_GPLL) : -ENOENT;
		if (!ret && clk_get_rate(gpll) == UMS_T760_GPLL)
			g->rates[5] = UMS_T760_GPLL / 5 * 2;
		else
			dev_warn(g->dev, "GPLL not at %luHz (%d): no 655.2MHz\n",
				 UMS_T760_GPLL, ret);
	}
	dev_info(g->dev, "T760 GPU bin %u at %duV: up to %luHz\n",
		 bin, g->voltage_uv, ums_max_rate(g));
	return 0;
}

/* Nothing binds the GPU devfreq cooling device to a thermal zone in the
 * vendor DT. Cap the GPU at 384MHz while gpu-thmzone is hot, like the stock
 * power allocator starting at the 70C passive trip.
 */
static int ums9620_hot_mc = 75000;
module_param(ums9620_hot_mc, int, 0644);
MODULE_PARM_DESC(ums9620_hot_mc, "gpu-thmzone millicelsius at which the GPU is capped at 384MHz");
static int ums9620_cool_mc = 65000;
module_param(ums9620_cool_mc, int, 0644);
MODULE_PARM_DESC(ums9620_cool_mc, "gpu-thmzone millicelsius at which the 384MHz cap is released");
#define UMS_THERMAL_POLL_MS 1000

static int ums_thermal_bind(struct ums_gpu *g)
{
	struct device_node *zone;
	struct of_phandle_args args;
	struct platform_device *supplier;
	struct device_link *link;
	int ret;

	zone = of_find_node_by_path("/thermal-zones/gpu-thmzone");
	if (!zone)
		return -ENODEV;
	ret = of_parse_phandle_with_args(zone, "thermal-sensors",
					 "#thermal-sensor-cells", 0, &args);
	of_node_put(zone);
	if (ret)
		return ret;
	supplier = of_find_device_by_node(args.np);
	of_node_put(args.np);
	if (!supplier)
		return -EPROBE_DEFER;
	/* The thermal API returns a borrowed zone pointer. Keep its provider
	 * bound until GPU devres has stopped this work, including during probe.
	 * A link created after supplier unbind began cannot protect the zone.
	 */
	link = device_link_add(g->dev, &supplier->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&supplier->dev);
	if (!link)
		return -EINVAL;
	if (READ_ONCE(link->status) != DL_STATE_CONSUMER_PROBE)
		return -EPROBE_DEFER;
	g->tz = thermal_zone_get_zone_by_name("gpu-thmzone");
	if (IS_ERR(g->tz)) {
		ret = PTR_ERR(g->tz);
		return ret == -ENODEV ? -EPROBE_DEFER : ret;
	}
	return 0;
}

static void ums_thermal_poll(struct work_struct *work)
{
	struct ums_gpu *g = container_of(to_delayed_work(work), struct ums_gpu,
					 thermal_work);
	int temp = 0, ret;
	bool capped;

	ret = thermal_zone_get_temp(g->tz, &temp);
	if (ret && !g->sensor_fault)
		dev_warn(g->dev, "GPU temperature unavailable (%d): limiting to %luHz\n",
			 ret, g->rates[3]);
	else if (!ret && g->sensor_fault)
		dev_info(g->dev, "GPU temperature recovered: %dmC\n", temp);
	g->sensor_fault = !!ret;
	capped = ums_thermal_cap_required(READ_ONCE(g->hot), ret, temp,
					 READ_ONCE(ums9620_hot_mc),
					 READ_ONCE(ums9620_cool_mc));
	if (capped != READ_ONCE(g->hot)) {
		ret = dev_pm_qos_update_request(&g->thermal_qos, capped ?
			g->rates[3] / 1000 : PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
		if (ret < 0) {
			dev_err_ratelimited(g->dev, "GPU thermal limit update failed: %d\n", ret);
		} else {
			WRITE_ONCE(g->hot, capped);
			if (capped)
				dev_info(g->dev, "GPU thermal cap: %luHz\n", g->rates[3]);
			else
				dev_info(g->dev, "GPU at %dmC: cap released\n", temp);
		}
	}
	queue_delayed_work(system_freezable_power_efficient_wq, &g->thermal_work,
			   msecs_to_jiffies(UMS_THERMAL_POLL_MS));
}

static void ums_thermal_stop(void *data)
{
	struct ums_gpu *g = data;

	cancel_delayed_work_sync(&g->thermal_work);
	dev_pm_qos_remove_request(&g->thermal_qos);
}

static int ums_thermal_start(struct ums_gpu *g)
{
	int ret;

	if (ums_max_rate(g) <= g->rates[3])
		return 0;
	ret = ums_thermal_bind(g);
	if (ret)
		return dev_err_probe(g->dev, ret, "GPU temperature provider unavailable\n");
	/* Do not allow high OPPs until a valid cool sample has been observed. */
	ret = dev_pm_qos_add_request(g->dev, &g->thermal_qos, DEV_PM_QOS_MAX_FREQUENCY,
					     g->rates[3] / 1000);
	if (ret < 0)
		return ret;
	WRITE_ONCE(g->hot, true);
	INIT_DEFERRABLE_WORK(&g->thermal_work, ums_thermal_poll);
	queue_delayed_work(system_freezable_power_efficient_wq, &g->thermal_work, 0);
	return devm_add_action_or_reset(g->dev, ums_thermal_stop, g);
}

int panfrost_ums9620_add_opps(struct panfrost_device *pfdev)
{
	struct ums_gpu *g = pfdev->platform_data;
	int i, ret;

	for (i = UMS_IDX_LO; i <= UMS_IDX_HI; i++) {
		if (!g->rates[i])
			continue;
		ret = dev_pm_opp_add(pfdev->dev, g->rates[i], 0);
		if (ret)
			return ret;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(panfrost_ums9620_add_opps);

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
	if (ums9620_freq_index != 2 && ums9620_freq_index != 3)
		return -EINVAL;
	g->rates[2] = 153600000;
	g->rates[3] = 384000000;
	g->rate = g->rates[ums9620_freq_index];
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
	ret = ums_t760_rates(g);
	if (ret) return ret;
	ret = ums_register_core_clock(g, pfdev);
	if (ret) return ret;
	pfdev->bus_clock = g->clk[0];
	ret = ums_thermal_start(g);
	if (ret) return ret;
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
