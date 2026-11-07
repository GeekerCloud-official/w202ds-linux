// SPDX-License-Identifier: GPL-2.0-only
/* W202DS dual UPM6910H: guarded main-only charging, standard power_supply ABI.
 * Probe stops both chips; userspace explicitly selects auto on the main.
 * Input ceilings follow the source contract and stock Android charge profile.
 * Keep HIZ, GPIO, OTG and the power path inherited. Never enable
 * the unconfigured slave. Cable events and the existing 2 s worker check the
 * battery NTC/FGU, charger faults and the bounded normal-charge profile.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include "charger.h"

static unsigned int max_input_mv = 12000;
module_param(max_input_mv, uint, 0444);
MODULE_PARM_DESC(max_input_mv, "UPM6910H input OVP preparation: 5000, 9000 or 12000 (default) mV; not a PD voltage request");

static char *pps_supply = "tcpm-source-psy-64400000.spi:pmic@0:pd@e00";
module_param(pps_supply, charp, 0444);
MODULE_PARM_DESC(pps_supply, "TCPM power_supply for PPS; defaults to this board's controller, empty disables the client");

static bool auto_pps = true;
module_param(auto_pps, bool, 0444);
MODULE_PARM_DESC(auto_pps, "Prefer PPS automatically during guarded auto charging (default on); 0 only renews manually activated PPS");

static void stop_work(struct work_struct *work)
{
	struct w202_bq *bq = container_of(to_delayed_work(work), struct w202_bq, stop_work);
	int ret, status, config, state, battery_current, pps_ret;
	bool changed = false;

	/* Do not hold the charger lock across a synchronous TCPM negotiation. */
	pps_ret = w202_bq_refresh_pps(bq);
	mutex_lock(&bq->lock);
	/* A link transition is a temporary safety stop, not a user's request
	 * to inhibit charging. Preserve auto across unplug/replug and retry
	 * only after the PPS client confirms a usable contract again.
	 */
	bq->pps_inhibited = !!pps_ret;
	ret = w202_bq_charge_locked(bq);
	/* The board does not yet provide a charger IRQ. Reuse this worker,
	 * rather than requiring userspace to refresh cached AC state. Compare
	 * PG, charging state, enable and cached policy health. Fault sampling
	 * belongs exclusively to w202_bq_charge_locked(), never to status/UI reads.
	 */
	status = i2c_smbus_read_byte_data(bq->client, 8);
	config = i2c_smbus_read_byte_data(bq->client, 1);
	if (status >= 0 && config >= 0) {
		state = (status & GENMASK(4, 2)) | ((config & CHARGE_ENABLE) << 8) |
			(bq->health << 16);
		changed = state != bq->last_state;
		bq->last_state = state;
	}
	if (!bq->slave && !w202_bq_battery_property(POWER_SUPPLY_PROP_CURRENT_NOW, &battery_current)) {
		state = battery_current > 50000 ? 1 : battery_current < -50000 ? -1 : 0;
		changed |= state != bq->battery_state;
		bq->battery_state = state;
	}
	changed |= bq->input_limit != bq->last_input_limit;
	bq->last_input_limit = bq->input_limit;
	if (!bq->shutting_down && !bq->suspended)
		schedule_delayed_work(&bq->stop_work, msecs_to_jiffies(2000));
	mutex_unlock(&bq->lock);
	if (changed)
		power_supply_changed(bq->psy);
	if (changed && !bq->slave)
		w202_bq_notify_supply("w202-battery");
	if (changed && !bq->slave)
		w202_bq_notify_supply("w202-usb-c");
	if (ret)
		dev_err_ratelimited(&bq->client->dev, "Stop not confirmed: %d\n", ret);
}

static int cable_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct w202_bq *bq = container_of(nb, struct w202_bq, cable_nb);

	/* Preserve a brief disconnect even if synchronous TCPM work outlasts
	 * the whole off/on interval. The worker owns the policy-state reset.
	 */
	if (!event)
		atomic_set(&bq->cable_detached, 1);
	/* Extcon may notify in atomic context. Read the charger in work context,
	 * after the PG signal has had a short settling interval.
	 */
	if (!READ_ONCE(bq->shutting_down) && !READ_ONCE(bq->suspended))
		mod_delayed_work(system_wq, &bq->stop_work, msecs_to_jiffies(50));
	return NOTIFY_OK;
}

static int watch_cable(struct w202_bq *bq)
{
	struct device_node *node, *upstream;
	struct extcon_dev *edev;
	int ret;

	node = of_parse_phandle(bq->client->dev.of_node, "extcon", 0);
	if (!node)
		return 0;
	edev = extcon_find_edev_by_node(node);
	/* The inherited main-charger graph points through the vendor Type-C
	 * node to the GPIO VBUS detector. Use that declared detector while the
	 * Type-C provider is absent, without changing the firmware DT.
	 */
	if (IS_ERR(edev)) {
		upstream = of_parse_phandle(node, "extcon", 0);
		if (upstream) {
			edev = extcon_find_edev_by_node(upstream);
			of_node_put(upstream);
		}
	}
	of_node_put(node);
	if (IS_ERR(edev))
		return dev_err_probe(&bq->client->dev, PTR_ERR(edev), "Cable detector\n");
	bq->cable_nb.notifier_call = cable_event;
	ret = devm_extcon_register_notifier_all(&bq->client->dev, edev, &bq->cable_nb);
	if (ret)
		return ret;
	bq->extcon = edev;
	dev_info(&bq->client->dev, "Cable events from %s\n", extcon_get_edev_name(edev));
	return 0;
}

static int source_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct w202_bq *bq = container_of(nb, struct w202_bq, source_nb);
	struct power_supply *psy = data;

	if (event == PSY_EVENT_PROP_CHANGED &&
	    (!strcmp(psy->desc->name, "w202-usb-c") ||
	     !strncmp(psy->desc->name, "tcpm-source-psy-", 16)) &&
	    !READ_ONCE(bq->shutting_down) && !READ_ONCE(bq->suspended))
		mod_delayed_work(system_wq, &bq->stop_work, 0);
	return NOTIFY_OK;
}

static void unwatch_source(void *data)
{
	struct w202_bq *bq = data;

	power_supply_unreg_notifier(&bq->source_nb);
}

static int w202_bq_probe(struct i2c_client *client)
{
	struct w202_bq *bq;
	int id, ret, charge_config;

	if (max_input_mv != 5000 && max_input_mv != 9000 && max_input_mv != 12000)
		return -EINVAL;
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
	bq->max_input_uv = max_input_mv * 1000;
	bq->charge_current_ua = MAX_CHARGE_CURRENT_UA;
	bq->pps_supply = pps_supply && *pps_supply && max_input_mv > 5000 ? pps_supply : NULL;
	bq->auto_pps = auto_pps;
	atomic_set(&bq->cable_detached, 0);
	bq->last_state = -1;
	bq->stop_requested = true;
	bq->health = POWER_SUPPLY_HEALTH_UNKNOWN;
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
	ret = w202_bq_stop_locked(bq);
	if (ret)
		return dev_err_probe(&client->dev, ret, "Initial stop not confirmed\n");
	if (bq->device_id == 0x14 && bq->vendor_id == 5) {
		ret = w202_bq_prepare_input_locked(bq);
		if (ret)
			return dev_err_probe(&client->dev, ret, "Input OVP not confirmed\n");
	} else if (max_input_mv != 5000) {
		return -EOPNOTSUPP;
	}
	ret = w202_bq_register_supply(bq);
	if (ret)
		return ret;
	charge_config = i2c_smbus_read_byte_data(client, 1);
	if (charge_config < 0)
		return charge_config;
	ret = watch_cable(bq);
	if (ret)
		return ret;
	bq->source_nb.notifier_call = source_event;
	ret = power_supply_reg_notifier(&bq->source_nb);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(&client->dev, unwatch_source, bq);
	if (ret)
		return ret;
	dev_info(&client->dev, "%s %s id=%02x vendor_id=%d CHG_CONFIG=%d; guarded charge control ready\n",
		 bq->chip_name, bq->slave ? "slave" : "main", id, bq->vendor_id,
		 !!(charge_config & CHARGE_ENABLE));
	schedule_delayed_work(&bq->stop_work, 0);
	return 0;
}

static void w202_bq_shutdown(struct i2c_client *client)
{
	struct w202_bq *bq = i2c_get_clientdata(client);

	mutex_lock(&bq->lock);
	bq->shutting_down = true;
	mutex_unlock(&bq->lock);
	if (bq->extcon)
		devm_extcon_unregister_notifier_all(&client->dev, bq->extcon, &bq->cable_nb);
	cancel_delayed_work_sync(&bq->stop_work);
	mutex_lock(&bq->lock);
	if (w202_bq_stop_locked(bq))
		dev_err(&client->dev, "Final stop not confirmed\n");
	mutex_unlock(&bq->lock);
	if (w202_bq_leave_pps(bq))
		dev_warn(&client->dev, "PPS exit not confirmed\n");
}

static int w202_bq_suspend(struct device *dev)
{
	struct w202_bq *bq = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&bq->lock);
	bq->suspended = true;
	ret = w202_bq_stop_locked(bq);
	mutex_unlock(&bq->lock);
	cancel_delayed_work_sync(&bq->stop_work);
	if (!ret)
		ret = w202_bq_leave_pps(bq);
	if (ret) {
		WRITE_ONCE(bq->suspended, false);
		mod_delayed_work(system_wq, &bq->stop_work, 0);
	}
	return ret;
}

static int w202_bq_resume(struct device *dev)
{
	struct w202_bq *bq = dev_get_drvdata(dev);

	WRITE_ONCE(bq->suspended, false);
	mod_delayed_work(system_wq, &bq->stop_work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(w202_bq_pm, w202_bq_suspend, w202_bq_resume);

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
		.pm = pm_sleep_ptr(&w202_bq_pm),
	},
	.probe = w202_bq_probe,
	.remove = w202_bq_shutdown,
	.shutdown = w202_bq_shutdown,
};
module_i2c_driver(w202_bq_driver);
MODULE_DESCRIPTION("W202DS UPM6910H guarded main charging and dual stop control");
MODULE_AUTHOR("W202DS BSP contributors");
MODULE_LICENSE("GPL");
