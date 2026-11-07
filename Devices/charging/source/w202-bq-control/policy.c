// SPDX-License-Identifier: GPL-2.0-only
/* Main-only charging limits, protection and requested operating mode. */
#include "charger.h"

static int input_limit_locked(struct w202_bq *bq)
{
	int available, ret, source_ret, nominal_uv, board_limit;

	source_ret = w202_bq_supply_property("w202-usb-c", POWER_SUPPLY_PROP_CURRENT_MAX, &available);
	/* Always reconcile the system input, including during manual stop.
	 * Removal/unknown providers return to 500 mA before another attachment.
	 */
	/* Stock Android battery DT: DCP input 2 A, fchg/flash input 1.25 A.
	 * REG00 uses 100 mA steps, so the fast-charge ceiling becomes 1.2 A.
	 * The PD contract advertises source capability; it is not a demand
	 * that this buck charger draw all of that current.
	 */
	ret = w202_bq_supply_property("w202-usb-c", POWER_SUPPLY_PROP_VOLTAGE_MAX, &nominal_uv);
	board_limit = !ret && nominal_uv <= 6500000 ? 2000000 : 1250000;
	bq->input_limit = source_ret || available < 100000 ? 500000 :
		min(available, board_limit) / 100000 * 100000;
	ret = i2c_smbus_read_byte_data(bq->client, 0);
	if (ret < 0)
		return ret;
	if ((ret & 0x1f) != bq->input_limit / 100000 - 1) {
		ret = w202_bq_stop_locked(bq);
		if (!ret)
			ret = w202_bq_update_locked(bq, 0, 0x1f, bq->input_limit / 100000 - 1);
		if (ret)
			return ret;
	}
	return source_ret ? source_ret : available < 100000 ? -ENODEV : 0;
}

int w202_bq_charge_locked(struct w202_bq *bq)
{
	int status, input, config, timer, path, old_fault, fault, ret, max_uv, max_ua;
	int current_config, voltage_config, term_config, input_config, source_uv, input_uv;
	int charge_current;
	int health = POWER_SUPPLY_HEALTH_UNKNOWN;

	if (bq->slave || bq->suspended)
		return w202_bq_stop_locked(bq);
	if (bq->device_id != 0x14 || bq->vendor_id != 5)
		goto inhibit;
	ret = input_limit_locked(bq);
	if (ret)
		goto inhibit;
	if (bq->stop_requested || bq->pps_inhibited)
		return w202_bq_stop_locked(bq);
	ret = w202_bq_prepare_input_locked(bq);
	if (!ret)
		ret = w202_bq_supply_property("w202-usb-c", POWER_SUPPLY_PROP_VOLTAGE_MAX, &source_uv);
	if (!ret)
		ret = w202_bq_supply_property("w202-usb-c", POWER_SUPPLY_PROP_VOLTAGE_NOW, &input_uv);
	if (ret)
		goto inhibit;
	if (source_uv > bq->max_input_uv || input_uv > bq->max_input_uv * 11 / 10) {
		health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		goto inhibit;
	}
	ret = w202_bq_battery_property(POWER_SUPPLY_PROP_TEMP, &bq->temperature);
	if (!ret)
		ret = w202_bq_battery_property(POWER_SUPPLY_PROP_VOLTAGE_NOW, &bq->voltage);
	if (!ret)
		ret = w202_bq_battery_property(POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX, &max_uv);
	if (!ret)
		ret = w202_bq_battery_property(POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, &max_ua);
	if (ret)
		goto inhibit;
	/* A conservative subset of the board's JEITA table: 10..43 C, with
	 * 3 C recovery hysteresis. No reduced-voltage warm/cold charging yet.
	 */
	if (bq->temperature < 100 || bq->temperature > 430)
		bq->thermal_blocked = true;
	else if (bq->temperature >= 130 && bq->temperature <= 400)
		bq->thermal_blocked = false;
	if (bq->thermal_blocked) {
		health = bq->temperature < 130 ? POWER_SUPPLY_HEALTH_COLD :
			POWER_SUPPLY_HEALTH_OVERHEAT;
		goto inhibit;
	}
	/* The default maximum current is confined to the central normal-temperature
	 * band, below stock's 15..45 C high-current JEITA interval. Retain the
	 * conservative 1.5 A profile outside it, and never enable the slave.
	 */
	charge_current = min(bq->charge_current_ua,
		bq->temperature >= 180 && bq->temperature <= 400 ?
		MAX_CHARGE_CURRENT_UA : CHARGE_CURRENT_UA);
	if (bq->voltage < 3000000 || bq->voltage > max_uv ||
	    max_uv < CHARGE_VOLTAGE_UV || max_ua < charge_current) {
		health = bq->voltage < 3000000 ? POWER_SUPPLY_HEALTH_DEAD :
			POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		goto inhibit;
	}
	status = i2c_smbus_read_byte_data(bq->client, 8);
	input = i2c_smbus_read_byte_data(bq->client, 10);
	config = i2c_smbus_read_byte_data(bq->client, 1);
	timer = i2c_smbus_read_byte_data(bq->client, 5);
	path = i2c_smbus_read_byte_data(bq->client, 7);
	input_config = i2c_smbus_read_byte_data(bq->client, 0);
	if (status < 0 || input < 0 || config < 0 || timer < 0 || path < 0 || input_config < 0)
		goto inhibit;
	if (!(status & BIT(2)) || !(input & BIT(7)))
		goto inhibit;
	/* Refuse HIZ, OTG, disabled BATFET or an unexpected charge watchdog.
	 * A watchdog expiry resets registers; it is NOT fail-safe charge-off.
	 */
	if ((input_config & BIT(7)) || (config & BIT(5)) || (path & BIT(5)) || (timer & 0x30))
		goto inhibit;
	/* Only the policy worker reads REG09. Save latched evidence before the
	 * second read obtains the current fault; UI reads use this cache.
	 */
	old_fault = i2c_smbus_read_byte_data(bq->client, 9);
	fault = i2c_smbus_read_byte_data(bq->client, 9);
	if (old_fault < 0 || fault < 0)
		goto inhibit;
	bq->fault_history |= old_fault | fault;
	bq->fault = fault;
	/* Latch watchdog, boost, battery OVP and safety-timer faults. A new
	 * explicit stop/start is required, never an automatic enable loop.
	 */
	if (((old_fault | fault) & 0xc8) ||
	    ((old_fault & 0x30) == 0x30) || ((fault & 0x30) == 0x30))
		bq->fault_latched = true;
	if (bq->fault_latched || fault) {
		health = (fault & BIT(3)) ? POWER_SUPPLY_HEALTH_OVERVOLTAGE :
			(fault & 0x30) == 0x30 ? POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE :
			POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		goto inhibit;
	}
	/* Reconcile after replug/reset using the source's live input limit.
	 * 4.400 V is below the pack's 4.430 V maximum; the old
	 * 4.432 V setting rounded above it. Retain hardware charge termination,
	 * enable its 10 h timer and lower die thermal regulation to 90 C.
	 */
	current_config = i2c_smbus_read_byte_data(bq->client, 2);
	term_config = i2c_smbus_read_byte_data(bq->client, 3);
	voltage_config = i2c_smbus_read_byte_data(bq->client, 4);
	if (current_config < 0 || term_config < 0 || voltage_config < 0)
		goto inhibit;
	/* Reprogram a changed/reset profile only while charging is off. */
	if ((current_config & 0x3f) != charge_current / 60000 ||
	    (term_config & 0x0f) != TERM_CURRENT_UA / 60000 - 1 ||
	    (voltage_config & 0xf8) != ((CHARGE_VOLTAGE_UV - 3856000) / 32000) << 3 ||
	    (timer & 0x8e) != 0x8c) {
		ret = w202_bq_stop_locked(bq);
		if (ret)
			goto inhibit;
	}
	ret = w202_bq_update_locked(bq, 2, 0x3f, charge_current / 60000);
	if (!ret)
		ret = w202_bq_update_locked(bq, 3, 0x0f, TERM_CURRENT_UA / 60000 - 1);
	if (!ret)
		ret = w202_bq_update_locked(bq, 4, 0xf8, ((CHARGE_VOLTAGE_UV - 3856000) / 32000) << 3);
	if (!ret)
		ret = w202_bq_update_locked(bq, 5, 0x8e, 0x8c);
	if (!ret)
		ret = w202_bq_update_locked(bq, 1, CHARGE_ENABLE | WATCHDOG_STROBE, CHARGE_ENABLE);
	if (!ret) {
		bq->health = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	}
inhibit:
	bq->health = health;
	return w202_bq_stop_locked(bq);
}

/* Shared by the standard charge_behaviour ABI and legacy stop_charge sysfs. */
int w202_bq_set_stopped(struct w202_bq *bq, bool stopped)
{
	int ret;

	if (!stopped && (bq->slave || bq->device_id != 0x14 || bq->vendor_id != 5))
		return -EOPNOTSUPP;
	mutex_lock(&bq->lock);
	if (bq->stop_requested && !stopped)
		bq->fault_latched = false;
	bq->stop_requested = stopped;
	ret = w202_bq_charge_locked(bq);
	mutex_unlock(&bq->lock);
	mod_delayed_work(system_wq, &bq->stop_work, 0);
	power_supply_changed(bq->psy);
	return ret;
}
