// SPDX-License-Identifier: GPL-2.0-only
/* W202DS telemetry for the FGU left running by stock LK/Android.
 * Register layout/calibration: stock sc27xx_fuel_gauge.c; see README.md.
 * Inherit the counter without resetting it. Only the Android RTC SOC banks
 * are written, after readback validation; no IRQ or charger policy changes.
 * The inherited Type-C/BC1.2 state also supplies standard USB source limits;
 * reading an Rp advertisement does not establish a USB PD contract.
 * ADC conversions and eFuse reads use the existing IIO/nvmem providers.
 */
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include "w202_fgu_soc.h"
#include "w202_usb_source.h"

#define FGU_VOLTAGE 0x20
#define FGU_CURRENT 0x2c
#define FGU_COUNTER_HI 0x68
#define FGU_COUNTER_LO 0x6c
#define FGU_SAVED_SOC 0xa8
#define FGU_NORMAL_SOC 0xc8
#define FGU_SOC_MASK 0xfff
#define SOC_SAMPLE_INTERVAL (10 * HZ)
#define CURRENT_ZERO 8192
#define SAMPLE_HZ 2

struct w202_power {
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *ntc;
	struct iio_channel *input_voltage;
	struct mutex lock;
	u32 base, resistance, full_uah, design_uah, energy_reference_uv;
	u32 typec_base;
	char *tcpm_supply_name;
	int voltage_gain, current_gain;
	u32 *vt;
	int vt_count;
	bool anchored;
	u32 counter_anchor, full_voltage, full_current;
	int soc_anchor;
	struct power_supply *battery;
	struct delayed_work soc_work;
	struct w202_fgu_full_state full_state;
	unsigned long last_save;
	int last_saved_soc, save_error;
	bool full_calibrated;
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
	u32 saved, normal, counter;
	s64 delta;
	int ret, soc;

	ret = read_counter(p, &counter);
	if (ret)
		return ret;
	if (!p->anchored) {
		ret = regmap_read(p->regmap, p->base + FGU_SAVED_SOC, &saved);
		if (ret)
			return ret;
		soc = w202_fgu_decode_soc(saved);
		if ((saved >> 12) != 5 || soc < 0)
			return -ENODATA;
		/* Android keeps UI SOC in bank 0 and unscaled SOC in bank 1.
		 * Prefer the latter for coulomb integration when available.
		 */
		ret = regmap_read(p->regmap, p->base + FGU_NORMAL_SOC, &normal);
		if (ret)
			return ret;
		if (w202_fgu_decode_soc(normal) >= 0)
			soc = w202_fgu_decode_soc(normal);
		p->counter_anchor = counter;
		p->soc_anchor = soc;
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

/* Android's SET/CLEAR handshake, preserving the upper mode/status nibble.
 * Never leave CLEAR asserted on an I/O error. Verify the complete word so
 * a damaged mode field is not silently accepted as a successful save.
 */
static int save_soc_bank(struct w202_power *p, u32 status_reg, int soc)
{
	u32 before, after, encoded = w202_fgu_encode_soc(soc);
	int ret, release;

	ret = regmap_read(p->regmap, p->base + status_reg, &before);
	if (ret || (before & FGU_SOC_MASK) == encoded)
		return ret;
	ret = regmap_write_bits(p->regmap, p->base + status_reg - 4,
				FGU_SOC_MASK, FGU_SOC_MASK);
	if (!ret) {
		usleep_range(200, 250);
		ret = regmap_write_bits(p->regmap, p->base + status_reg - 8,
					FGU_SOC_MASK, encoded);
		usleep_range(200, 250);
	}
	release = regmap_write_bits(p->regmap, p->base + status_reg - 4, FGU_SOC_MASK, 0);
	usleep_range(200, 250);
	if (ret || release)
		return ret ? ret : release;
	ret = regmap_read(p->regmap, p->base + status_reg, &after);
	if (!ret && after != ((before & ~FGU_SOC_MASK) | encoded))
		ret = -EIO;
	return ret;
}

/* Caller holds only the telemetry lock, never a charger lock. */
static int save_soc(struct w202_power *p, int soc)
{
	u32 saved;
	int ret;

	ret = regmap_read(p->regmap, p->base + FGU_SAVED_SOC, &saved);
	if (!ret && (saved >> 12) != 5)
		ret = -ENODATA; /* Do not change first-boot or firmware-owned mode. */
	if (!ret)
		ret = save_soc_bank(p, FGU_NORMAL_SOC, soc);
	if (!ret)
		ret = save_soc_bank(p, FGU_SAVED_SOC, soc);
	p->save_error = ret;
	if (!ret) {
		p->last_saved_soc = soc;
		p->last_save = jiffies;
	}
	return ret;
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

static int input_voltage_uv(struct w202_power *p, int *value)
{
	int mv, ret;

	if (IS_ERR(p->input_voltage))
		return PTR_ERR(p->input_voltage);
	ret = iio_read_channel_processed(p->input_voltage, &mv);
	if (ret < 0)
		return ret;
	if (mv < 0 || mv > 20000)
		return -ENODATA;
	*value = mv * 1000;
	return 0;
}

static void soc_work(struct work_struct *work)
{
	struct w202_power *p = container_of(to_delayed_work(work), struct w202_power, soc_work);
	struct power_supply *charger;
	union power_supply_propval status = {}, health = {};
	u32 counter = 0;
	int voltage = 0, battery_ua = 0, temp = 0, online = 0, soc, ret, sample_ret;
	bool charger_ok = false, eligible;

	/* Keep charger -> telemetry lock order, as in get_property(). */
	charger = power_supply_get_by_name("w202-bq-main");
	if (charger) {
		charger_ok = !power_supply_get_property(charger, POWER_SUPPLY_PROP_STATUS, &status) &&
			!power_supply_get_property(charger, POWER_SUPPLY_PROP_HEALTH, &health);
		power_supply_put(charger);
	}
	mutex_lock(&p->lock);
	sample_ret = voltage_uv(p, &voltage);
	if (!sample_ret)
		sample_ret = current_ua(p, &battery_ua);
	if (!sample_ret)
		sample_ret = temperature_decic(p, &temp);
	if (!sample_ret)
		sample_ret = external_power(p, &online);
	if (!sample_ret)
		sample_ret = read_counter(p, &counter);
	eligible = !sample_ret && charger_ok && w202_fgu_full_eligible(online,
		status.intval == POWER_SUPPLY_STATUS_FULL,
		health.intval == POWER_SUPPLY_HEALTH_GOOD,
		voltage, battery_ua, temp, p->full_voltage, p->full_current);
	if (w202_fgu_confirm_full(&p->full_state, eligible, jiffies, 30 * HZ, 20 * HZ)) {
		/* Like Android adjust_cap(): establish a new software anchor,
		 * leaving the continuously running hardware counter untouched.
		 */
		p->soc_anchor = 1000;
		p->counter_anchor = counter;
		p->anchored = p->full_calibrated = true;
		p->last_saved_soc = -1;
		dev_info(p->dev, "FGU full calibration: %duV, %duA, temp=%d, counter=%u\n",
			 voltage, battery_ua, temp, counter);
	}
	ret = capacity_tenths(p, &soc);
	if (!ret && (p->last_saved_soc < 0 ||
		    time_after_eq(jiffies, p->last_save + 60 * HZ))) {
		ret = save_soc(p, soc);
		if (ret)
			dev_warn_ratelimited(p->dev, "FGU SOC checkpoint failed: %d\n", ret);
	}
	mutex_unlock(&p->lock);
	power_supply_changed(p->battery);
	queue_delayed_work(system_freezable_wq, &p->soc_work, SOC_SAMPLE_INTERVAL);
}

static int w202_usb_get_property(struct power_supply *psy,
				enum power_supply_property property,
				union power_supply_propval *val)
{
	struct w202_power *p = power_supply_get_drvdata(psy);
	struct power_supply *source;
	union power_supply_propval limit, online, usb_type;
	u32 raw, status = 0, debug = 0, enable = 0;
	int ret;

	ret = regmap_read(p->regmap, 0x239c, &raw);
	if (ret)
		return ret;
	switch (property) {
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		return 0;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = !!(raw & BIT(2));
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = 5000000;
		source = p->tcpm_supply_name ? power_supply_get_by_name(p->tcpm_supply_name) : NULL;
		if (source) {
			/* TCPM VOLTAGE_NOW is the negotiated nominal target. Its
			 * VOLTAGE_MAX in PPS mode is the source APDO ceiling, not
			 * the present input; our VOLTAGE_NOW remains the ADC.
			 */
			ret = power_supply_get_property(source, POWER_SUPPLY_PROP_VOLTAGE_NOW, val);
			power_supply_put(source);
			return ret;
		}
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return input_voltage_uv(p, &val->intval);
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = POWER_SUPPLY_USB_TYPE_C;
		source = p->tcpm_supply_name ? power_supply_get_by_name(p->tcpm_supply_name) : NULL;
		if (source) {
			ret = power_supply_get_property(source, property, val);
			power_supply_put(source);
			return ret;
		}
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		if (!(raw & BIT(2))) {
			val->intval = 0;
			return 0;
		}
		if (p->typec_base) {
			ret = regmap_read(p->regmap, p->typec_base, &enable);
			if (!ret)
				ret = regmap_read(p->regmap, p->typec_base + 0x1c, &status);
			if (!ret)
				ret = regmap_read(p->regmap, p->typec_base + 0x60, &debug);
			if (ret)
				return ret;
			/* Vendor sc27xx_pd: state 2 = attached sink; STATUS[7:6]
			 * is live Rp (0 default, 1 1.5 A, 2 3 A). DBG1[8] is
			 * VBUS_OK. Never use a stale detached/source-state code.
			 */
		}
		val->intval = w202_usb_source_current(raw, enable, status, debug);
		/* TCPM owns the source's negotiated budget. Once it manages this
		 * port, a PD contract can be lower than Rp, or exceed default Rp.
		 * Never keep using a 3 A advertisement over a smaller contract.
		 */
		source = p->tcpm_supply_name ? power_supply_get_by_name(p->tcpm_supply_name) : NULL;
		if (source) {
			ret = power_supply_get_property(source, POWER_SUPPLY_PROP_ONLINE, &online);
			if (!ret)
				ret = power_supply_get_property(source, POWER_SUPPLY_PROP_CURRENT_NOW, &limit);
			if (!ret)
				ret = power_supply_get_property(source, POWER_SUPPLY_PROP_USB_TYPE, &usb_type);
			power_supply_put(source);
			if (ret)
				return ret;
			if (!online.intval || limit.intval <= 0)
				val->intval = 0;
			else
				val->intval = w202_usb_sink_budget(val->intval, limit.intval,
					usb_type.intval == POWER_SUPPLY_USB_TYPE_PD ||
					usb_type.intval == POWER_SUPPLY_USB_TYPE_PD_PPS);
		}
		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property w202_usb_properties[] = {
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static const struct power_supply_desc w202_usb_desc = {
	.name = "w202-usb-c",
	.type = POWER_SUPPLY_TYPE_USB,
	/* TCPM reports source PPS capability even with a fixed-PDO contract. */
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_C) | BIT(POWER_SUPPLY_USB_TYPE_PD) |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS),
	.properties = w202_usb_properties,
	.num_properties = ARRAY_SIZE(w202_usb_properties),
	.get_property = w202_usb_get_property,
};

static int w202_get_property(struct power_supply *psy,
			     enum power_supply_property property,
			     union power_supply_propval *val)
{
	struct w202_power *p = power_supply_get_drvdata(psy);
	struct power_supply *charger;
	union power_supply_propval charger_status = { .intval = POWER_SUPPLY_STATUS_UNKNOWN };
	int ret = 0, battery_current, battery_voltage, external, capacity;

	/* Charger policy reads our voltage/temperature with its lock held.
	 * Read the optional standard charger status BEFORE taking p->lock;
	 * never create the opposite telemetry -> charger lock order.
	 */
	if (property == POWER_SUPPLY_PROP_STATUS) {
		charger = power_supply_get_by_name("w202-bq-main");
		if (charger) {
			if (power_supply_get_property(charger, POWER_SUPPLY_PROP_STATUS,
						      &charger_status))
				charger_status.intval = POWER_SUPPLY_STATUS_UNKNOWN;
			power_supply_put(charger);
		}
	}
	mutex_lock(&p->lock);
	switch (property) {
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = voltage_uv(p, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = current_ua(p, &val->intval);
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		ret = voltage_uv(p, &battery_voltage);
		if (!ret)
			ret = current_ua(p, &battery_current);
		if (!ret)
			val->intval = div_s64((s64)battery_voltage * battery_current, 1000000);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		ret = capacity_tenths(p, &capacity);
		if (!ret) {
			val->intval = property == POWER_SUPPLY_PROP_CAPACITY ?
				DIV_ROUND_CLOSEST(capacity, 10) :
				DIV_ROUND_CLOSEST_ULL((u64)capacity * p->full_uah, 1000);
			if (property == POWER_SUPPLY_PROP_ENERGY_NOW)
				val->intval = DIV_ROUND_CLOSEST_ULL((u64)val->intval *
					p->energy_reference_uv, 1000000);
		}
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
				external && charger_status.intval == POWER_SUPPLY_STATUS_FULL ?
				POWER_SUPPLY_STATUS_FULL :
				external ? POWER_SUPPLY_STATUS_NOT_CHARGING : POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = p->full_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = p->design_uah;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL:
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		/* Match UPower's existing fixed-reference charge-to-energy estimate.
		 * Only POWER_NOW uses live V * I; these are not measured Wh values.
		 */
		val->intval = DIV_ROUND_CLOSEST_ULL((u64)p->energy_reference_uv *
			(property == POWER_SUPPLY_PROP_ENERGY_FULL ? p->full_uah : p->design_uah),
			1000000);
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
	POWER_SUPPLY_PROP_SCOPE, POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_ENERGY_NOW, POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
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
	struct w202_power *p = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&p->lock);
	ret = sysfs_emit(buf, "%s; Android RTC checkpoints, no counter reset\n",
		p->full_calibrated ? "full-calibrated-coulomb" : "rtc-soc-plus-coulomb (baseline unverified)");
	mutex_unlock(&p->lock);
	return ret;
}
static DEVICE_ATTR_RO(capacity_source);

static ssize_t soc_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_power *p = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&p->lock);
	ret = sysfs_emit(buf, "anchored=%u anchor_soc=%d counter_anchor=%u full_calibrated=%u "
		"full_tracking=%d last_saved_soc=%d save_error=%d full_uv=%u full_ua=%u\n",
		p->anchored, p->soc_anchor, p->counter_anchor, p->full_calibrated,
		p->full_state.tracking, p->last_saved_soc, p->save_error,
		p->full_voltage, p->full_current);
	mutex_unlock(&p->lock);
	return ret;
}
static DEVICE_ATTR_RO(soc_state);

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
	int value, ret = input_voltage_uv(p, &value);

	return ret ? ret : sysfs_emit(buf, "%d\n", value);
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
	&dev_attr_soc_state.attr,
	&dev_attr_external_power.attr, &dev_attr_input_voltage_uv.attr,
	&dev_attr_coulomb_counter.attr, NULL,
};
ATTRIBUTE_GROUPS(w202);

static int w202_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_power *p;
	struct nvmem_cell *cell;
	struct device_node *battery, *typec;
	struct device_node *pd_node;
	struct platform_device *pd_device;
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
	if (!ret)
		ret = of_property_read_u32(battery, "voltage-min-design-microvolt", &p->energy_reference_uv);
	if (!ret)
		ret = of_property_read_u32(battery, "fullbatt-voltage", &p->full_voltage);
	if (!ret)
		ret = of_property_read_u32(battery, "fullbatt-current", &p->full_current);
	p->vt_count = of_property_count_u32_elems(battery, "voltage-temp-table");
	if (!ret && (p->full_uah != 7286000 || p->design_uah < p->full_uah ||
		     p->energy_reference_uv < 2000000 || p->energy_reference_uv > 5500000 ||
		     p->full_voltage < 4200000 || p->full_voltage > 4500000 ||
		     p->full_current < 50000 || p->full_current > 420000 ||
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
	typec = of_parse_phandle(dev->of_node, "extcon", 0);
	if (typec && typec->parent == dev->of_node->parent &&
	    of_device_is_compatible(typec, "sprd,ump96xx-typec"))
		of_property_read_u32(typec, "reg", &p->typec_base);
	of_node_put(typec);
	pd_node = of_get_child_by_name(dev->of_node->parent, "pd");
	if (pd_node) {
		pd_device = of_find_device_by_node(pd_node);
		of_node_put(pd_node);
		if (!pd_device)
			return -EPROBE_DEFER;
		p->tcpm_supply_name = devm_kasprintf(dev, GFP_KERNEL, "tcpm-source-psy-%s",
			dev_name(&pd_device->dev));
		put_device(&pd_device->dev);
		if (!p->tcpm_supply_name)
			return -ENOMEM;
	}
	/* Establish the optional estimate before userspace can read the supply. */
	capacity_tenths(p, &capacity);
	config.drv_data = p;
	psy = devm_power_supply_register(dev, &w202_desc, &config);
	if (IS_ERR(psy))
		return PTR_ERR(psy);
	p->battery = psy;
	psy = devm_power_supply_register(dev, &w202_usb_desc, &config);
	if (IS_ERR(psy))
		return PTR_ERR(psy);
	p->last_saved_soc = -1;
	INIT_DELAYED_WORK(&p->soc_work, soc_work);
	queue_delayed_work(system_freezable_wq, &p->soc_work, SOC_SAMPLE_INTERVAL);
	dev_info(dev, "Inherited FGU: %duV, gains=%d/%d, SOC=%s, RTC checkpoints enabled\n",
		 voltage, p->voltage_gain, p->current_gain,
		 p->anchored ? "RTC estimate" : "unavailable");
	return 0;
}

static void w202_power_stop(struct platform_device *pdev)
{
	struct w202_power *p = platform_get_drvdata(pdev);
	int soc, ret;

	cancel_delayed_work_sync(&p->soc_work);
	mutex_lock(&p->lock);
	ret = capacity_tenths(p, &soc);
	if (!ret)
		ret = save_soc(p, soc);
	mutex_unlock(&p->lock);
	if (ret)
		dev_warn(p->dev, "Final FGU SOC checkpoint failed: %d\n", ret);
}

static const struct of_device_id w202_power_of_match[] = {
	{ .compatible = "sprd,ump9620-fgu" }, {}
};
MODULE_DEVICE_TABLE(of, w202_power_of_match);

static struct platform_driver w202_power_driver = {
	.probe = w202_power_probe,
	.remove = w202_power_stop,
	.shutdown = w202_power_stop,
	.driver = {
		.name = "w202-ump9620-power",
		.of_match_table = w202_power_of_match,
		.dev_groups = w202_groups,
	},
};
module_platform_driver(w202_power_driver);
MODULE_DESCRIPTION("W202DS UMP9620 FGU telemetry and Android-compatible SOC checkpoints");
MODULE_AUTHOR("W202DS BSP contributors");
MODULE_LICENSE("GPL");
