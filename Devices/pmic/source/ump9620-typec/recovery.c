// SPDX-License-Identifier: GPL-2.0
/* Bounded protocol recovery; never reset the machine or source VBUS. */
#include "controller.h"

/* Two consecutive resets are enough to restart a stale source session.
 * Refill only after a contract survives 60 seconds, or CC really detaches.
 * PPS renewals must not restart the stable-contract interval.
 */
int w202_pd_begin_reset(struct w202_typec *pd)
{
	int ret;

	lockdep_assert_held(&pd->lock);
	if (pd->faulted)
		return -ESHUTDOWN;
	/* A bouncing detector must not refill the physical reset budget.
	 * This ceiling survives cable events, unlike the per-session budget.
	 */
	if (!pd->reset_window_valid ||
	    time_after_eq(jiffies, pd->reset_window_start + 60 * HZ)) {
		pd->reset_window_start = jiffies;
		pd->reset_window_attempts = 0;
		pd->reset_window_valid = true;
	}
	if (pd->contract_valid &&
	    time_after_eq(jiffies, pd->contract_since + 60 * HZ))
		pd->reset_attempts = 0;
	pd->contract_valid = false;
	if (pd->reset_attempts >= 2 || pd->reset_window_attempts >= 2) {
		w202_pd_latch_fault(pd, "hard-reset-budget-exhausted");
		return -ESHUTDOWN;
	}
	pd->reset_attempts++;
	pd->reset_window_attempts++;
	pd->reset_count++;
	pd->nr_source_caps = 0;
	ret = w202_pd_flush(pd, true);
	if (!ret)
		ret = regmap_update_bits(pd->regmap,
			pd->typec_base + SC27XX_TYPEC_PD_CFG,
			SC27XX_TYPEC_PD_CONSTRACT, 0);
	if (ret)
		w202_pd_latch_fault(pd, "hard-reset-io-error");
	return ret;
}

void w202_pd_contract_established(struct w202_typec *pd)
{
	lockdep_assert_held(&pd->lock);
	if (!pd->contract_valid) {
		pd->contract_since = jiffies;
		pd->contract_valid = true;
	}
}

/* Called only on a detach IRQ with the detector in DETACHED_SNK.
 * PD faults disable PD IRQs, not Type-C detection. Keep lifecycle ready so
 * that a real unplug can rearm TCPM without unloading modules.
 */
int w202_pd_detached(struct w202_typec *pd)
{
	int ret;

	lockdep_assert_held(&pd->lock);
	pd->contract_valid = false;
	pd->reset_attempts = 0;
	pd->nr_source_caps = 0;
	/* Stock sc27xx_pd_check_vbus_cc_status resets FIFOs and both IDs
	 * on cable changes, even when there was no prior protocol fault.
	 */
	ret = w202_pd_flush(pd, true);
	if (!ret)
		ret = regmap_update_bits(pd->regmap,
			pd->typec_base + SC27XX_TYPEC_PD_CFG,
			SC27XX_TYPEC_PD_CONSTRACT, 0);
	if (ret) {
		w202_pd_latch_fault(pd, "detach-io-error");
		return ret;
	}
	pd->tx_pending = false;
	if (!pd->faulted)
		return 0;
	if (!ret)
		ret = regmap_write(pd->regmap, pd->base + SC27XX_INT_CLR,
			SC27XX_INT_CLR_MASK);
	if (!ret)
		ret = regmap_write(pd->regmap, pd->base + SC27XX_INT_EN,
			SC27XX_INT_EN_MASK);
	if (ret)
		return ret;
	pd->faulted = false;
	pd->fault_reason = NULL;
	pd->rearm_count++;
	return 1;
}

int w202_pd_cc_event(struct w202_typec *pd, u32 events, u32 status)
{
	lockdep_assert_held(&pd->lock);
	/* Stock detector state 0 is DETACHED_SNK; state 2 is ATTACHED_SNK.
	 * DBG1[9] has no validated detach semantics in the stock driver.
	 * An attached sample or VBUS-only event never unlocks a PD fault.
	 */
	if (!(events & BIT(1)) || (status & SC27XX_STATE_MASK))
		return 0;
	return w202_pd_detached(pd);
}
