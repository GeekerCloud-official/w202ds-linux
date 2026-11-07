// SPDX-License-Identifier: GPL-2.0-only
/* Standard TCPM PPS client, using the existing charging policy worker.
 * Prefer PPS during auto charging; no separate timer or userspace daemon.
 * Only this worker owns PPS retry/deadline state. Never hold the charger lock
 * across a TCPM setter: negotiation emits notifications that read the charger.
 */
#include <linux/jiffies.h>
#include "charger.h"
#include "pps-limits.h"

int w202_bq_leave_pps(struct w202_bq *bq)
{
	struct power_supply *source;
	union power_supply_propval online;
	int ret;

	if (bq->slave || !bq->pps_supply)
		return 0;
	source = power_supply_get_by_name(bq->pps_supply);
	if (!source)
		return 0;
	ret = power_supply_get_property(source, POWER_SUPPLY_PROP_ONLINE, &online);
	if (!ret && online.intval == 2) {
		online.intval = 1;
		ret = power_supply_set_property(source, POWER_SUPPLY_PROP_ONLINE, &online);
	}
	power_supply_put(source);
	return ret;
}

static bool auto_allowed(struct w202_bq *bq)
{
	bool allowed;

	mutex_lock(&bq->lock);
	allowed = !bq->stop_requested && !bq->suspended && !bq->shutting_down &&
		!bq->thermal_blocked && !bq->fault_latched &&
		bq->health == POWER_SUPPLY_HEALTH_GOOD;
	mutex_unlock(&bq->lock);
	return allowed;
}

static int retry_exit(struct w202_bq *bq)
{
	int ret;

	bq->pps_exit_pending = true;
	if (bq->pps_next_refresh && time_before(jiffies, bq->pps_next_refresh))
		return -EAGAIN;
	/* An unsuccessful setter also emits a source notification. Pace exits
	 * as well as renewals; otherwise EAGAIN can trigger a notifier loop.
	 */
	bq->pps_next_refresh = jiffies + msecs_to_jiffies(2000);
	ret = w202_bq_leave_pps(bq);
	if (!ret)
		bq->pps_exit_pending = false;
	return ret;
}

int w202_bq_refresh_pps(struct w202_bq *bq)
{
	struct power_supply *source;
	union power_supply_propval online, voltage, amperage, value, minimum, maximum;
	int ret, input_online, target, fallback;

	if (bq->slave || !bq->pps_supply)
		return 0;
	if (atomic_xchg(&bq->cable_detached, 0)) {
		bq->pps_blocked = false;
		bq->pps_exit_pending = false;
		bq->pps_retries = 0;
		bq->pps_next_refresh = 0;
	}
	source = power_supply_get_by_name(bq->pps_supply);
	if (!source) {
		bq->pps_blocked = false;
		bq->pps_exit_pending = false;
		bq->pps_next_refresh = 0;
		return 0;
	}
	ret = power_supply_get_property(source, POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret)
		goto fail;
	if (!online.intval) {
		bq->pps_next_refresh = 0;
		bq->pps_retries = 0;
		bq->pps_blocked = false;
		bq->pps_exit_pending = false;
		goto out;
	}
	if (online.intval != 2)
		bq->pps_exit_pending = false;
	/* Check disconnect first so a faulted/thermally blocked attachment also
	 * resets its PPS latch. Manual stop always wins, including one arriving
	 * during negotiation. Protection gates leave the user's auto request intact.
	 */
	if (bq->auto_pps && !auto_allowed(bq)) {
		if (READ_ONCE(bq->stop_requested))
			bq->pps_blocked = false;
		bq->pps_retries = 0;
		power_supply_put(source);
		if (online.intval == 2)
			return retry_exit(bq);
		bq->pps_next_refresh = jiffies + msecs_to_jiffies(2000);
		return 0;
	}
	if (bq->pps_exit_pending) {
		power_supply_put(source);
		return retry_exit(bq);
	}
	if (bq->pps_next_refresh && time_before(jiffies, bq->pps_next_refresh))
		goto out;
	/* Set before any setter can notify source_event. Those notifications
	 * may schedule an immediate pass, but cannot trigger a busy retry loop.
	 */
	bq->pps_next_refresh = jiffies + msecs_to_jiffies(2000);
	if (online.intval != 2) {
		if (!bq->auto_pps || bq->pps_blocked)
			goto out;
		ret = power_supply_get_property(source, POWER_SUPPLY_PROP_USB_TYPE, &value);
		if (ret)
			goto fail;
		if (value.intval != POWER_SUPPLY_USB_TYPE_PD_PPS)
			goto out;
		value.intval = 2;
		ret = power_supply_set_property(source, POWER_SUPPLY_PROP_ONLINE, &value);
		if (ret)
			goto fail;
		dev_info(&bq->client->dev, "Automatic PPS active; board ceiling %d uV\n",
			 min(bq->max_input_uv, W202_PPS_MAX_UV));
		goto negotiated;
	}
	ret = w202_bq_supply_property("w202-usb-c", POWER_SUPPLY_PROP_ONLINE, &input_online);
	if (!ret && !input_online)
		ret = -ENOLINK;
	if (ret)
		goto fail;
	ret = power_supply_get_property(source, POWER_SUPPLY_PROP_VOLTAGE_NOW, &voltage);
	if (!ret)
		ret = power_supply_get_property(source, POWER_SUPPLY_PROP_CURRENT_MAX, &amperage);
	if (!ret && (voltage.intval < 5000000 ||
		     voltage.intval > min(bq->max_input_uv, 11000000) || amperage.intval <= 0))
		ret = -ERANGE;
	if (ret)
		goto fail;
	amperage.intval = min(amperage.intval, 3000000) / 50000 * 50000;
	if (bq->auto_pps && !bq->pps_blocked) {
		ret = power_supply_get_property(source, POWER_SUPPLY_PROP_VOLTAGE_MIN, &minimum);
		if (!ret)
			ret = power_supply_get_property(source, POWER_SUPPLY_PROP_VOLTAGE_MAX, &maximum);
		if (!ret)
			ret = power_supply_get_property(source, POWER_SUPPLY_PROP_CURRENT_NOW, &value);
		if (ret)
			goto fail;
		target = w202_pps_next_uv(voltage.intval, minimum.intval, maximum.intval,
					 bq->max_input_uv);
		if (target < 0) {
			ret = target;
			goto fail;
		}
		/* First request the available budget, then raise voltage in 200 mV
		 * steps. The UPM input limit still follows the stock 1.25 A table.
		 */
		if (value.intval == amperage.intval && voltage.intval != target) {
			value.intval = target;
			ret = power_supply_set_property(source, POWER_SUPPLY_PROP_VOLTAGE_NOW, &value);
			goto negotiated;
		}
	}
	/* Renew current, never write a stale sampled voltage back to TCPM. */
	bq->pps_next_refresh = jiffies + msecs_to_jiffies(4000);
	ret = power_supply_set_property(source, POWER_SUPPLY_PROP_CURRENT_NOW, &amperage);
negotiated:
	if (ret)
		goto fail;
	bq->pps_retries = 0;
	if (bq->auto_pps && !auto_allowed(bq)) {
		power_supply_put(source);
		return retry_exit(bq);
	}
	goto out;
fail:
	if (ret == -EAGAIN && ++bq->pps_retries < 5)
		goto out_retry;
	/* At most one failed PPS attempt per attachment. Fixed charging can
	 * resume only after the regular worker confirms PG, ADC and protection.
	 */
	bq->pps_blocked = true;
	bq->pps_next_refresh = 0;
	power_supply_put(source);
	fallback = retry_exit(bq);
	dev_warn(&bq->client->dev, "PPS policy failed: %d, fixed fallback: %d; PPS blocked until disconnect or stop/start\n",
		 ret, fallback);
	return fallback;
out_retry:
	ret = 0;
out:
	power_supply_put(source);
	return ret;
}
