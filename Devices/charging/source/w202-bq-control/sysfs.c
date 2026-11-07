// SPDX-License-Identifier: GPL-2.0-only
/* Read-only diagnostics and the legacy stop_charge compatibility ABI. */
#include "charger.h"

static ssize_t stop_charge_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct w202_bq *bq = dev_get_drvdata(dev);
	bool stopped;
	int ret;

	ret = kstrtobool(buf, &stopped);
	if (ret)
		return ret;
	ret = w202_bq_set_stopped(bq, stopped);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(stop_charge);

static ssize_t chip_identity_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_bq *bq = dev_get_drvdata(dev);

	/* Immutable probe snapshot; status reads do not touch extended registers. */
	if (bq->vendor_id < 0)
		return sysfs_emit(buf, "chip=%s role=%s reg0b=%02x rega7=unavailable(%d)\n",
				  bq->chip_name, bq->slave ? "slave" : "main",
				  bq->device_id, bq->vendor_id);
	return sysfs_emit(buf, "chip=%s role=%s reg0b=%02x rega7=%02x\n",
			  bq->chip_name, bq->slave ? "slave" : "main",
			  bq->device_id, bq->vendor_id);
}
static DEVICE_ATTR_RO(chip_identity);

static ssize_t charge_enabled_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_bq *bq = dev_get_drvdata(dev);
	int value;

	mutex_lock(&bq->lock);
	value = i2c_smbus_read_byte_data(bq->client, 1);
	mutex_unlock(&bq->lock);
	return value < 0 ? value : sysfs_emit(buf, "%d\n", !!(value & CHARGE_ENABLE));
}
static DEVICE_ATTR_RO(charge_enabled);

static ssize_t configuration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_bq *bq = dev_get_drvdata(dev);
	int values[12] = {}, ret = 0, i, length = 0, enable = -1;

	mutex_lock(&bq->lock);
	for (i = 0; i < ARRAY_SIZE(values); i++) {
		/* REG09 fault is latched; do not clear it as a status side effect. */
		if (i == 9)
			continue;
		values[i] = i2c_smbus_read_byte_data(bq->client, i);
		if (values[i] < 0) {
			ret = values[i];
			break;
		}
	}
	if (!ret && bq->enable) {
		enable = gpiod_get_value_cansleep(bq->enable);
		if (enable < 0)
			ret = enable;
	}
	mutex_unlock(&bq->lock);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(values); i++)
		if (i != 9)
			length += sysfs_emit_at(buf, length, "reg%02x=%02x ", i, values[i]);
	return length + sysfs_emit_at(buf, length, "enable_gpio=%d\n", enable);
}
static DEVICE_ATTR_RO(configuration);

static ssize_t charging_policy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_bq *bq = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&bq->lock);
	ret = sysfs_emit(buf, "auto=%d suspended=%d temp_decic=%d voltage_uv=%d thermal_blocked=%d fault=%02x history=%02x latched=%d health=%d input_limit_ua=%d requested_charge_ua=%d auto_pps=%d pps_blocked=%d pps_exit_pending=%d\n",
		!bq->stop_requested, bq->suspended, bq->temperature, bq->voltage,
		bq->thermal_blocked, bq->fault, bq->fault_history, bq->fault_latched, bq->health,
		bq->input_limit, bq->charge_current_ua, bq->auto_pps, READ_ONCE(bq->pps_blocked),
		READ_ONCE(bq->pps_exit_pending));
	mutex_unlock(&bq->lock);
	return ret;
}
static DEVICE_ATTR_RO(charging_policy);

static struct attribute *w202_bq_attrs[] = {
	&dev_attr_stop_charge.attr,
	&dev_attr_charge_enabled.attr,
	&dev_attr_configuration.attr,
	&dev_attr_chip_identity.attr,
	&dev_attr_charging_policy.attr,
	NULL,
};

static const struct attribute_group w202_bq_group = {
	.attrs = w202_bq_attrs,
};

const struct attribute_group *w202_bq_groups[] = {
	&w202_bq_group,
	NULL,
};
