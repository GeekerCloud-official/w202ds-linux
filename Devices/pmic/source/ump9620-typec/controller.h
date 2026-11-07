/* SPDX-License-Identifier: GPL-2.0 */
#ifndef W202_UMP9620_TYPEC_H
#define W202_UMP9620_TYPEC_H

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/usb/tcpm.h>
#include "registers.h"

struct w202_saved_reg {
	u32 address, value;
};

struct w202_typec {
	struct device *dev;
	struct device_node *typec_node;
	struct regmap *regmap;
	struct regmap *aon_apb;
	u32 saved_aon_clock;
	bool aon_clock_saved;
	struct mutex lock;
	struct tcpc_dev tcpc;
	struct tcpm_port *port;
	struct power_supply *supply;
	u32 base, typec_base, rc_cal, ref_cal, delta_cal;
	u32 source_caps[PDO_MAX_OBJECTS];
	unsigned int nr_source_caps, max_mv, max_ma;
	bool allow_pps;
	bool reset_on_probe;
	u64 block_reset_count;
	struct w202_saved_reg saved[24];
	unsigned int saved_count;
	int pd_irq, typec_irq;
	bool active, ready, tx_pending, tx_hard_reset, faulted;
	const char *fault_reason;
	const char *last_fault_reason;
	unsigned int reset_attempts;
	unsigned int reset_window_attempts;
	unsigned long reset_window_start;
	bool reset_window_valid;
	unsigned long contract_since;
	bool contract_valid;
	u64 reset_count, fault_count, rearm_count;
	u64 cc_irq_count;
	u32 last_cc_events, last_cc_status, last_cc_debug;
	u8 tx_id;
	u32 last_irq_flags;
	u64 rx_count, tx_count, irq_count, error_count, tx_complete_count;
	u64 blocked_requests;
};

static inline struct w202_typec *tcpc_to_w202(struct tcpc_dev *tcpc)
{
	return container_of(tcpc, struct w202_typec, tcpc);
}

int w202_typec_snapshot(struct w202_typec *pd);
void w202_typec_restore(struct w202_typec *pd);
int w202_typec_calibrate(struct w202_typec *pd);
int w202_typec_hw_init(struct w202_typec *pd);
int w202_typec_enable_clock(struct w202_typec *pd);
int w202_typec_reset_block(struct w202_typec *pd);
int w202_typec_supply_register(struct w202_typec *pd);
void w202_typec_supply_unregister(struct w202_typec *pd);
int w202_pd_flush(struct w202_typec *pd, bool reset_ids);
void w202_pd_latch_fault(struct w202_typec *pd, const char *reason);
int w202_pd_begin_reset(struct w202_typec *pd);
void w202_pd_contract_established(struct w202_typec *pd);
int w202_pd_detached(struct w202_typec *pd);
int w202_pd_cc_event(struct w202_typec *pd, u32 events, u32 status);
int w202_pd_set_rx(struct tcpc_dev *tcpc, bool on);
int w202_pd_transmit(struct tcpc_dev *tcpc, enum tcpm_transmit_type type,
		     const struct pd_message *msg, unsigned int negotiated_rev);
irqreturn_t w202_pd_irq(int irq, void *data);

#endif
