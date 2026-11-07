// SPDX-License-Identifier: GPL-2.0-only
/* Standard power_supply properties and access to existing PMIC supplies. */
#include "charger.h"

int w202_bq_supply_property(const char *name, enum power_supply_property property, int *value)
{
	struct power_supply *battery = power_supply_get_by_name(name);
	union power_supply_propval val;
	int ret;

	if (!battery)
		return -ENODEV;
	ret = power_supply_get_property(battery, property, &val);
	if (!ret)
		*value = val.intval;
	power_supply_put(battery);
	return ret;
}

int w202_bq_battery_property(enum power_supply_property property, int *value)
{
	return w202_bq_supply_property("w202-battery", property, value);
}

void w202_bq_notify_supply(const char *name)
{
	struct power_supply *battery = power_supply_get_by_name(name);

	if (battery) {
		power_supply_changed(battery);
		power_supply_put(battery);
	}
}

static int get_property(struct power_supply *psy, enum power_supply_property property,
			union power_supply_propval *val)
{
	struct w202_bq *bq = power_supply_get_drvdata(psy);
	int status, config, ret = 0, state;

	if (property == POWER_SUPPLY_PROP_MODEL_NAME) {
		val->strval = bq->model_name;
		return 0;
	}
	if (property == POWER_SUPPLY_PROP_SCOPE) {
		/* Internal charger chips; w202-usb-c is the system AC input. */
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		return 0;
	}
	mutex_lock(&bq->lock);
	if (property == POWER_SUPPLY_PROP_VOLTAGE_MAX) {
		ret = i2c_smbus_read_byte_data(bq->client, 6);
		if (ret >= 0) {
			if ((ret & 0xc0) != w202_bq_ovp_bits(bq))
				ret = -EIO;
			else {
				val->intval = bq->max_input_uv;
				ret = 0;
			}
		}
		goto out;
	}
	if (property == POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR) {
		val->intval = bq->stop_requested ? POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE :
			POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;
		goto out;
	}
	if (property == POWER_SUPPLY_PROP_HEALTH) {
		val->intval = bq->health;
		goto out;
	}
	if (property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX) {
		val->intval = bq->slave ? 0 : MAX_CHARGE_CURRENT_UA;
		goto out;
	}
	if (property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
	    property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ||
	    property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE) {
		int reg = property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ? 0 :
			property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT ? 2 : 4;

		ret = i2c_smbus_read_byte_data(bq->client, reg);
		if (ret >= 0) {
			val->intval = reg == 0 ? ((ret & 0x1f) + 1) * 100000 :
				reg == 2 ? (ret & 0x3f) * 60000 :
				(ret >> 3) == 15 ? 4352000 : 3856000 + (ret >> 3) * 32000;
			ret = 0;
		}
		goto out;
	}
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
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR, POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
};

static int set_property(struct power_supply *psy, enum power_supply_property property,
			const union power_supply_propval *val)
{
	struct w202_bq *bq = power_supply_get_drvdata(psy);
	int ret;

	if (property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT) {
		if (bq->slave || bq->device_id != 0x14 || bq->vendor_id != 5)
			return -EOPNOTSUPP;
		if (val->intval < 60000 || val->intval > MAX_CHARGE_CURRENT_UA)
			return -ERANGE;
		mutex_lock(&bq->lock);
		bq->charge_current_ua = val->intval / 60000 * 60000;
		ret = w202_bq_charge_locked(bq);
		mutex_unlock(&bq->lock);
		power_supply_changed(bq->psy);
		return ret;
	}

	if (property != POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR)
		return -EINVAL;
	if (val->intval != POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO &&
	    val->intval != POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE)
		return -EINVAL;
	return w202_bq_set_stopped(bq,
		val->intval == POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE);
}

static int property_is_writeable(struct power_supply *psy, enum power_supply_property property)
{
	struct w202_bq *bq = power_supply_get_drvdata(psy);

	return property == POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR ||
		(property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT &&
		 !bq->slave && bq->device_id == 0x14 && bq->vendor_id == 5);
}

int w202_bq_register_supply(struct w202_bq *bq)
{
	struct power_supply_config config = {};

	bq->desc.name = bq->slave ? "w202-bq-slave" : "w202-bq-main";
	bq->desc.type = POWER_SUPPLY_TYPE_USB;
	bq->desc.properties = w202_bq_properties;
	bq->desc.num_properties = ARRAY_SIZE(w202_bq_properties);
	bq->desc.get_property = get_property;
	bq->desc.set_property = set_property;
	bq->desc.property_is_writeable = property_is_writeable;
	bq->desc.charge_behaviours = BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE);
	if (!bq->slave && bq->device_id == 0x14 && bq->vendor_id == 5)
		bq->desc.charge_behaviours |= BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO);
	config.drv_data = bq;
	bq->psy = devm_power_supply_register(&bq->client->dev, &bq->desc, &config);
	return PTR_ERR_OR_ZERO(bq->psy);
}
