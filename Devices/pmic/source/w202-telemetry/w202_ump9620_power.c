// SPDX-License-Identifier: GPL-2.0-only
/* W202DS telemetry for the FGU left running by stock LK/Android.
 * Register layout/calibration: vendor-reference/sc27xx_fuel_gauge.c.
 * No FGU writes, IRQ changes, counter resets, RTC SOC writes or charger policy.
 * ADC conversions and eFuse reads use the existing IIO/nvmem providers.
 */
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define FGU_VOLTAGE 0x20
#define FGU_CURRENT 0x2c
#define FGU_COUNTER_HI 0x68
#define FGU_COUNTER_LO 0x6c
#define FGU_SAVED_SOC 0xa8
#define CURRENT_ZERO 8192
#define SAMPLE_HZ 2

struct w202_power {
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *ntc;
	struct iio_channel *input_voltage;
	struct mutex lock;
	u32 base, resistance, full_uah, design_uah;
	int voltage_gain, current_gain;
	u32 *vt;
	int vt_count;
	bool anchored;
	u32 saved_anchor, counter_anchor;
	int soc_anchor;
};

static int read_stable(struct w202_power *p, u32 offset, u32 *value)
{
	u32 previous;
	int ret, i;

	ret = regmap_read(p->regmap, p->base + offset, &previous);
	if (ret)
		return ret;
	for (i = 0; i < 8; i++) {
		ret = regmap_read(p->regmap, p->base + offset, value);
		if (ret)
			return ret;
		if (*value == previous)
			return 0;
		previous = *value;
	}
	return -EAGAIN;
}

static int voltage_uv(struct w202_power *p, int *value)
{
	u32 raw;
	int ret = read_stable(p, FGU_VOLTAGE, &raw);

	if (ret)
		return ret;
	if (raw > 0x1fff)
		return -ENODATA;
	*value = DIV_ROUND_CLOSEST_ULL((u64)raw * 1000000, p->voltage_gain);
	return *value >= 2000000 && *value <= 5500000 ? 0 : -ENODATA;
}

static int current_ua(struct w202_power *p, int *value)
{
	u32 raw;
	int ret = read_stable(p, FGU_CURRENT, &raw);

	if (ret)
		return ret;
	if (raw > 0x3fff)
		return -ENODATA;
	*value = DIV_S64_ROUND_CLOSEST(((s64)raw - CURRENT_ZERO) * 1000000,
				     p->current_gain);
	return abs(*value) <= 12000000 ? 0 : -ENODATA;
}

static int read_counter(struct w202_power *p, u32 *counter)
{
	u32 hi, lo, verify;
	int i, ret;

	for (i = 0; i < 4; i++) {
		ret = regmap_read(p->regmap, p->base + FGU_COUNTER_HI, &hi);
		if (ret)
			return ret;
		ret = regmap_read(p->regmap, p->base + FGU_COUNTER_LO, &lo);
		if (ret)
			return ret;
		ret = regmap_read(p->regmap, p->base + FGU_COUNTER_HI, &verify);
		if (ret)
			return ret;
		if (hi == verify) {
			*counter = (hi << 16) | (lo & 0xffff);
			return 0;
		}
	}
	return -EAGAIN;
}

/* Tenths of one percent. The saved value can predate this Linux boot. */
static int capacity_tenths(struct w202_power *p, int *value)
{
	u32 saved, counter, whole, fraction;
	s64 delta;
	int ret;

	ret = regmap_read(p->regmap, p->base + FGU_SAVED_SOC, &saved);
	if (ret)
		return ret;
	whole = saved & 0xff;
	fraction = (saved >> 8) & 0xf;
	if ((saved >> 12) != 5 || (saved & 0xfff) == 0xfff ||
	    (saved & 0xfff) == 0xa05 || whole > 100 || fraction > 9 ||
	    (whole == 100 && fraction)) {
		p->anchored = false;
		return -ENODATA;
	}
	ret = read_counter(p, &counter);
	if (ret)
		return ret;
	if (!p->anchored || p->saved_anchor != saved) {
		p->saved_anchor = saved;
		p->counter_anchor = counter;
		p->soc_anchor = whole * 10 + fraction;
		p->anchored = true;
	}
	/* uAh = counts * 1000000 / (3600 * SAMPLE_HZ * current_gain).
	 * Convert to tenths of a percent against the stock charge-full value.
	 * Signed subtraction handles one counter wrap in either direction.
	 */
	delta = (s64)(s32)(counter - p->counter_anchor) * 1000000000LL;
	{
		s64 divisor = 3600LL * SAMPLE_HZ * p->current_gain * p->full_uah;

		delta = div64_s64(delta + (delta >= 0 ? divisor / 2 : -divisor / 2), divisor);
	}
	if (delta < -1000 || delta > 1000)
		return -ENODATA;
	*value = clamp_t(s64, p->soc_anchor + delta, 0, 1000);
	return 0;
}

static int temperature_decic(struct w202_power *p, int *value)
{
	int mv, ret, i;
	s64 uv, result;

	ret = iio_read_channel_processed(p->ntc, &mv);
	if (ret < 0)
		return ret;
	uv = (s64)mv * 1000;
	if (uv > p->vt[0] || uv < p->vt[p->vt_count - 2])
		return -ENODATA;
	for (i = 2; i < p->vt_count; i += 2) {
		if (uv >= p->vt[i]) {
			result = (s64)p->vt[i - 1] +
				div_s64(((s64)p->vt[i + 1] - p->vt[i - 1]) *
					(p->vt[i - 2] - uv),
					p->vt[i - 2] - p->vt[i]);
			*value = result - 1000;
			return 0;
		}
	}
	return -ENODATA;
}

static int external_power(struct w202_power *p, int *value)
{
	u32 raw;
	int ret = regmap_read(p->regmap, 0x239c, &raw);

	if (!ret)
		*value = !!(raw & BIT(2));
	return ret;
}

static int w202_get_property(struct power_supply *psy,
			     enum power_supply_property property,
			     union power_supply_propval *val)
{
	struct w202_power *p = power_supply_get_drvdata(psy);
	int ret = 0, battery_current, external, capacity;

	mutex_lock(&p->lock);
	switch (property) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = voltage_uv(p, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = current_ua(p, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = capacity_tenths(p, &capacity);
		if (!ret)
			val->intval = property == POWER_SUPPLY_PROP_CAPACITY ?
				DIV_ROUND_CLOSEST(capacity, 10) :
				DIV_ROUND_CLOSEST_ULL((u64)capacity * p->full_uah, 1000);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = temperature_decic(p, &val->intval);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = current_ua(p, &battery_current);
		if (!ret)
			ret = external_power(p, &external);
		if (!ret)
			val->intval = battery_current > 50000 ? POWER_SUPPLY_STATUS_CHARGING :
				battery_current < -50000 ? POWER_SUPPLY_STATUS_DISCHARGING :
				external ? POWER_SUPPLY_STATUS_NOT_CHARGING : POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = p->full_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = p->design_uah;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "W202DS battery (UMP9620 telemetry)";
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&p->lock);
	return ret;
}

static enum power_supply_property w202_properties[] = {
	POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW, POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc w202_desc = {
	.name = "w202-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = w202_properties,
	.num_properties = ARRAY_SIZE(w202_properties),
	.get_property = w202_get_property,
};

static ssize_t capacity_source_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "rtc-soc-plus-coulomb-delta (saved baseline may be stale)\n");
}
static DEVICE_ATTR_RO(capacity_source);

static ssize_t calibration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_power *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "voltage_gain=%d current_gain=%d resistance_uohm=%u\n",
			  p->voltage_gain, p->current_gain, p->resistance);
}
static DEVICE_ATTR_RO(calibration);

static ssize_t external_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_power *p = dev_get_drvdata(dev);
	int value, ret = external_power(p, &value);

	return ret ? ret : sysfs_emit(buf, "%d\n", value);
}
static DEVICE_ATTR_RO(external_power);

static ssize_t input_voltage_uv_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_power *p = dev_get_drvdata(dev);
	int mv, ret;

	if (IS_ERR(p->input_voltage))
		return PTR_ERR(p->input_voltage);
	ret = iio_read_channel_processed(p->input_voltage, &mv);
	if (ret < 0)
		return ret;
	if (mv < 0 || mv > 20000)
		return -ENODATA;
	return sysfs_emit(buf, "%d\n", mv * 1000);
}
static DEVICE_ATTR_RO(input_voltage_uv);

static ssize_t coulomb_counter_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_power *p = dev_get_drvdata(dev);
	u32 counter;
	int ret;

	mutex_lock(&p->lock);
	ret = read_counter(p, &counter);
	mutex_unlock(&p->lock);
	/* Raw inherited counter, not rounded SOC/charge_now. Differences use
	 * signed 32-bit wrap and the calibrated current_gain, at SAMPLE_HZ.
	 * No reset, fresh baseline, interrupt or FGU write is introduced.
	 */
	return ret ? ret : sysfs_emit(buf, "%u\n", counter);
}
static DEVICE_ATTR_RO(coulomb_counter);

static struct attribute *w202_attrs[] = {
	&dev_attr_capacity_source.attr, &dev_attr_calibration.attr,
	&dev_attr_external_power.attr, &dev_attr_input_voltage_uv.attr,
	&dev_attr_coulomb_counter.attr, NULL,
};
ATTRIBUTE_GROUPS(w202);

static int w202_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_power *p;
	struct nvmem_cell *cell;
	struct device_node *battery;
	struct power_supply_config config = {};
	struct power_supply *psy;
	u32 module, clock, calib;
	void *data;
	size_t length;
	int ret, i, voltage, capacity;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->regmap = dev_get_regmap(dev->parent, NULL);
	if (!p->regmap)
		return -EPROBE_DEFER;
	ret = of_property_read_u32(dev->of_node, "reg", &p->base);
	if (ret || p->base != 0x800)
		return -EINVAL;
	ret = of_property_read_u32(dev->of_node, "sprd,calib-resistance-micro-ohms", &p->resistance);
	if (ret || p->resistance != 4850)
		return -EINVAL;
	ret = regmap_read(p->regmap, 0x2008, &module);
	if (!ret)
		ret = regmap_read(p->regmap, 0x2010, &clock);
	if (ret)
		return ret;
	if (!(module & BIT(7)) || !(clock & BIT(6)))
		return dev_err_probe(dev, -ENODEV, "Inherited FGU is disabled; leaving it unchanged\n");
	cell = devm_nvmem_cell_get(dev, "fgu_calib");
	if (IS_ERR(cell))
		return dev_err_probe(dev, PTR_ERR(cell), "FGU eFuse calibration\n");
	data = nvmem_cell_read(cell, &length);
	if (IS_ERR(data))
		return PTR_ERR(data);
	if (length < 2) {
		kfree(data);
		return -EINVAL;
	}
	calib = (get_unaligned_le16(data) >> 7) & 0x1ff;
	kfree(data);
	p->voltage_gain = DIV_ROUND_CLOSEST((calib + 6963 - 4096 - 256) * 10, 42);
	p->current_gain = DIV_ROUND_CLOSEST(p->voltage_gain * 4 * p->resistance, 20000);
	if (p->voltage_gain < 500 || p->voltage_gain > 1500 || !p->current_gain)
		return -EINVAL;
	p->ntc = devm_iio_channel_get(dev, "bat-temp");
	if (IS_ERR(p->ntc))
		return dev_err_probe(dev, PTR_ERR(p->ntc), "Battery NTC channel\n");
	/* This board's DT names ADC channel 14 charge-vol; channel 6 is
	 * adc-batt-id and must never be treated as the external input voltage.
	 * A missing optional channel does not remove existing FGU telemetry.
	 */
	p->input_voltage = devm_iio_channel_get(dev, "charge-vol");
	if (IS_ERR(p->input_voltage) && PTR_ERR(p->input_voltage) == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	battery = of_parse_phandle(dev->of_node, "monitored-battery", 0);
	if (!battery)
		return -EINVAL;
	ret = of_property_read_u32(battery, "charge-full-microamp-hours", &p->full_uah);
	if (!ret)
		ret = of_property_read_u32(battery, "charge-full-design-microamp-hours", &p->design_uah);
	p->vt_count = of_property_count_u32_elems(battery, "voltage-temp-table");
	if (!ret && (p->full_uah != 7286000 || p->design_uah < p->full_uah ||
		     p->vt_count < 4 || p->vt_count > 512 || p->vt_count % 2))
		ret = -EINVAL;
	if (!ret) {
		p->vt = devm_kcalloc(dev, p->vt_count, sizeof(*p->vt), GFP_KERNEL);
		if (!p->vt)
			ret = -ENOMEM;
		else
			ret = of_property_read_u32_array(battery, "voltage-temp-table", p->vt, p->vt_count);
	}
	of_node_put(battery);
	if (ret)
		return ret;
	for (i = 2; i < p->vt_count; i += 2)
		if (p->vt[i] >= p->vt[i - 2] || p->vt[i + 1] <= p->vt[i - 1])
			return -EINVAL;
	ret = voltage_uv(p, &voltage);
	if (ret)
		return dev_err_probe(dev, ret, "No valid inherited battery voltage\n");
	mutex_init(&p->lock);
	platform_set_drvdata(pdev, p);
	/* Establish the optional estimate before userspace can read the supply. */
	capacity_tenths(p, &capacity);
	config.drv_data = p;
	psy = devm_power_supply_register(dev, &w202_desc, &config);
	if (IS_ERR(psy))
		return PTR_ERR(psy);
	dev_info(dev, "Read-only telemetry: %duV, gains=%d/%d, SOC=%s\n",
		 voltage, p->voltage_gain, p->current_gain,
		 p->anchored ? "RTC estimate" : "unavailable");
	return 0;
}

static const struct of_device_id w202_power_of_match[] = {
	{ .compatible = "sprd,ump9620-fgu" }, {}
};
MODULE_DEVICE_TABLE(of, w202_power_of_match);

static struct platform_driver w202_power_driver = {
	.probe = w202_power_probe,
	.driver = {
		.name = "w202-ump9620-power",
		.of_match_table = w202_power_of_match,
		.dev_groups = w202_groups,
	},
};
module_platform_driver(w202_power_driver);
MODULE_DESCRIPTION("W202DS UMP9620 inherited FGU telemetry, read-only");
MODULE_AUTHOR("W202DS BSP contributors");
MODULE_LICENSE("GPL");
