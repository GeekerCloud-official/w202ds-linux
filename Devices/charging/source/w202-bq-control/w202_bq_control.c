// SPDX-License-Identifier: GPL-2.0-only
/* W202DS dual BQ2560x-compatible chips: preserve configuration, stop control.
 * No probe/reset initialization, HIZ, current/voltage, GPIO or OTG writes.
 * Stock identifies UPM6910H using REG0B PN/sub-ID and a plain REG A7 read.
 * Both physical chips are matched. A requested stop is checked every 2 s.
 * Charging enable requires a separate, not-yet-implemented thermal policy.
 */
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define CHARGE_ENABLE BIT(4)
#define WATCHDOG_STROBE BIT(6)

struct w202_bq {
	struct i2c_client *client;
	struct gpio_desc *enable;
	struct mutex lock;
	struct delayed_work stop_work;
	struct power_supply_desc desc;
	struct power_supply *psy;
	const char *chip_name;
	char model_name[40];
	int device_id, vendor_id;
	bool stop_requested, slave, shutting_down;
};

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

static int stop_locked(struct w202_bq *bq)
{
	int value, verify, ret;

	value = i2c_smbus_read_byte_data(bq->client, 1);
	if (value < 0)
		return value;
	if (value & CHARGE_ENABLE) {
		ret = i2c_smbus_write_byte_data(bq->client, 1,
					      value & ~(CHARGE_ENABLE | WATCHDOG_STROBE));
		if (ret)
			return ret;
	}
	verify = i2c_smbus_read_byte_data(bq->client, 1);
	if (verify < 0)
		return verify;
	return verify & CHARGE_ENABLE ? -EIO : 0;
}

static void stop_work(struct work_struct *work)
{
	struct w202_bq *bq = container_of(to_delayed_work(work), struct w202_bq, stop_work);
	int ret;

	mutex_lock(&bq->lock);
	ret = stop_locked(bq);
	if (!bq->shutting_down)
		schedule_delayed_work(&bq->stop_work, msecs_to_jiffies(2000));
	mutex_unlock(&bq->lock);
	if (ret)
		dev_err_ratelimited(&bq->client->dev, "Stop not confirmed: %d\n", ret);
}

static ssize_t stop_charge_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct w202_bq *bq = dev_get_drvdata(dev);
	bool requested;
	int ret;

	ret = kstrtobool(buf, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EOPNOTSUPP;
	mutex_lock(&bq->lock);
	bq->stop_requested = true;
	ret = stop_locked(bq);
	mutex_unlock(&bq->lock);
	mod_delayed_work(system_wq, &bq->stop_work, msecs_to_jiffies(2000));
	power_supply_changed(bq->psy);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(stop_charge);

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

static struct attribute *w202_bq_attrs[] = {
	&dev_attr_stop_charge.attr, &dev_attr_charge_enabled.attr,
	&dev_attr_configuration.attr, &dev_attr_chip_identity.attr, NULL,
};
ATTRIBUTE_GROUPS(w202_bq);

static int get_property(struct power_supply *psy, enum power_supply_property property,
			union power_supply_propval *val)
{
	struct w202_bq *bq = power_supply_get_drvdata(psy);
	int status, config, ret = 0, state;

	if (property == POWER_SUPPLY_PROP_MODEL_NAME) {
		val->strval = bq->model_name;
		return 0;
	}
	mutex_lock(&bq->lock);
	status = i2c_smbus_read_byte_data(bq->client, 8);
	if (status < 0) {
		ret = status;
		goto out;
	}
	if (property == POWER_SUPPLY_PROP_ONLINE) {
		val->intval = !!(status & BIT(2));
		goto out;
	}
	if (property != POWER_SUPPLY_PROP_STATUS) {
		ret = -EINVAL;
		goto out;
	}
	config = i2c_smbus_read_byte_data(bq->client, 1);
	if (config < 0) {
		ret = config;
		goto out;
	}
	state = (status >> 3) & 3;
	val->intval = !(status & BIT(2)) || !(config & CHARGE_ENABLE) || !state ?
		POWER_SUPPLY_STATUS_NOT_CHARGING : state == 3 ?
		POWER_SUPPLY_STATUS_FULL : POWER_SUPPLY_STATUS_CHARGING;
out:
	mutex_unlock(&bq->lock);
	return ret;
}

static enum power_supply_property w202_bq_properties[] = {
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_MODEL_NAME,
};

static int w202_bq_probe(struct i2c_client *client)
{
	struct w202_bq *bq;
	struct power_supply_config config = {};
	int id, ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	id = i2c_smbus_read_byte_data(client, 0x0b);
	if (id < 0)
		return id;
	if (((id >> 3) & 0xf) != 2 && ((id >> 3) & 0xf) != 9)
		return -ENODEV;
	bq = devm_kzalloc(&client->dev, sizeof(*bq), GFP_KERNEL);
	if (!bq)
		return -ENOMEM;
	bq->client = client;
	bq->slave = of_property_read_bool(client->dev.of_node, "role-slave");
	bq->device_id = id;
	bq->vendor_id = -ENODATA;
	bq->chip_name = "BQ2560x-compatible";
	/* Exact test from this board's stock sqc_bq2560x.ko. REG0B alone
	 * is ambiguous. Do not assume another vendor on a failed A7 read.
	 * Stock read_no_record uses SMBus reads only; no unlock sequence.
	 */
	if (((id >> 3) & 0xf) == 2 && (id & BIT(2))) {
		bq->vendor_id = i2c_smbus_read_byte_data(client, 0xa7);
		if (bq->vendor_id == 5)
			bq->chip_name = "UPM6910H";
		else if (bq->vendor_id < 0)
			dev_warn(&client->dev, "Chip identity A7 unavailable: %d; retaining stop control\n",
				 bq->vendor_id);
	}
	snprintf(bq->model_name, sizeof(bq->model_name), "W202DS %s %s",
		 bq->chip_name, bq->slave ? "slave" : "main");
	bq->enable = devm_gpiod_get_optional(&client->dev, "enable", GPIOD_ASIS);
	if (IS_ERR(bq->enable))
		return dev_err_probe(&client->dev, PTR_ERR(bq->enable), "Inherited enable GPIO\n");
	mutex_init(&bq->lock);
	INIT_DELAYED_WORK(&bq->stop_work, stop_work);
	i2c_set_clientdata(client, bq);
	bq->desc.name = bq->slave ? "w202-bq-slave" : "w202-bq-main";
	bq->desc.type = POWER_SUPPLY_TYPE_USB;
	bq->desc.properties = w202_bq_properties;
	bq->desc.num_properties = ARRAY_SIZE(w202_bq_properties);
	bq->desc.get_property = get_property;
	config.drv_data = bq;
	bq->psy = devm_power_supply_register(&client->dev, &bq->desc, &config);
	if (IS_ERR(bq->psy))
		return PTR_ERR(bq->psy);
	ret = i2c_smbus_read_byte_data(client, 1);
	if (ret < 0)
		return ret;
	dev_info(&client->dev, "Preserved %s %s profile id=%02x vendor_id=%d CHG_CONFIG=%d; stop control ready\n",
		 bq->chip_name, bq->slave ? "slave" : "main", id, bq->vendor_id,
		 !!(ret & CHARGE_ENABLE));
	return 0;
}

static void w202_bq_shutdown(struct i2c_client *client)
{
	struct w202_bq *bq = i2c_get_clientdata(client);

	mutex_lock(&bq->lock);
	bq->shutting_down = true;
	mutex_unlock(&bq->lock);
	cancel_delayed_work_sync(&bq->stop_work);
	if (bq->stop_requested) {
		mutex_lock(&bq->lock);
		if (stop_locked(bq))
			dev_err(&client->dev, "Final stop not confirmed\n");
		mutex_unlock(&bq->lock);
	}
}

static const struct of_device_id w202_bq_match[] = {
	{ .compatible = "ti,bq2560x_chg" },
	{ .compatible = "ti,bq2560x_chg2" }, {}
};
MODULE_DEVICE_TABLE(of, w202_bq_match);

static struct i2c_driver w202_bq_driver = {
	.driver = {
		.name = "w202-bq-control",
		.of_match_table = w202_bq_match,
		.dev_groups = w202_bq_groups,
	},
	.probe = w202_bq_probe,
	.remove = w202_bq_shutdown,
	.shutdown = w202_bq_shutdown,
};
module_i2c_driver(w202_bq_driver);
MODULE_DESCRIPTION("W202DS dual UPM6910H/BQ2560x-compatible preserve-configuration stop control");
MODULE_AUTHOR("W202DS BSP contributors");
MODULE_LICENSE("GPL");
