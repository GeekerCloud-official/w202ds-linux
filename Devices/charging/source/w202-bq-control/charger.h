/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_BQ_CHARGER_H
#define W202_BQ_CHARGER_H

#include <linux/atomic.h>
#include <linux/extcon.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define CHARGE_ENABLE BIT(4)
#define WATCHDOG_STROBE BIT(6)
#define CHARGE_CURRENT_UA 1500000
/* Stock Android fchg_cur=2.65 A, rounded down to the 60 mA register step. */
#define MAX_CHARGE_CURRENT_UA 2640000
#define CHARGE_VOLTAGE_UV 4400000
#define TERM_CURRENT_UA 420000

struct w202_bq {
	struct i2c_client *client;
	struct gpio_desc *enable;
	struct mutex lock;
	struct delayed_work stop_work;
	struct extcon_dev *extcon;
	struct notifier_block cable_nb;
	struct notifier_block source_nb;
	struct power_supply_desc desc;
	struct power_supply *psy;
	const char *chip_name;
	char model_name[40];
	int device_id, vendor_id, last_state, last_input_limit;
	int health, fault, fault_history, temperature, voltage, battery_state, input_limit;
	int max_input_uv;
	int charge_current_ua;
	const char *pps_supply;
	unsigned long pps_next_refresh;
	/* PPS state is owned by stop_work, never written by a notifier/setter. */
	unsigned int pps_retries;
	atomic_t cable_detached;
	bool auto_pps, pps_blocked, pps_exit_pending;
	bool pps_inhibited; /* Under lock: temporary gate, never the user request. */
	bool stop_requested, slave, shutting_down, suspended, thermal_blocked, fault_latched;
};

/* Private to this composite module. The *_locked helpers require bq->lock.
 * IRQ/extcon context schedules work; only process/work context performs I2C.
 */
int w202_bq_stop_locked(struct w202_bq *bq);
int w202_bq_update_locked(struct w202_bq *bq, int reg, int mask, int bits);
int w202_bq_prepare_input_locked(struct w202_bq *bq);
static inline int w202_bq_ovp_bits(const struct w202_bq *bq)
{
	return (bq->max_input_uv <= 5000000 ? 1 :
		bq->max_input_uv <= 9000000 ? 2 : 3) << 6;
}
int w202_bq_charge_locked(struct w202_bq *bq);
int w202_bq_set_stopped(struct w202_bq *bq, bool stopped);
int w202_bq_refresh_pps(struct w202_bq *bq);
int w202_bq_leave_pps(struct w202_bq *bq);
int w202_bq_supply_property(const char *name, enum power_supply_property property, int *value);
int w202_bq_battery_property(enum power_supply_property property, int *value);
void w202_bq_notify_supply(const char *name);
int w202_bq_register_supply(struct w202_bq *bq);
extern const struct attribute_group *w202_bq_groups[];

#endif
