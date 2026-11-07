/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_FGU_SOC_H
#define W202_FGU_SOC_H

/* Android stores percent in bits 7:0 and tenths in bits 11:8.
 * Mode/status in bits 15:12 belongs to the boot firmware, not SOC.
 */
static inline int w202_fgu_decode_soc(unsigned int raw)
{
	unsigned int whole = raw & 0xff, tenth = (raw >> 8) & 0xf;

	if (whole > 100 || tenth > 9 || (whole == 100 && tenth))
		return -1;
	return whole * 10 + tenth;
}

static inline unsigned int w202_fgu_encode_soc(unsigned int soc)
{
	return soc / 10 | ((soc % 10) << 8);
}

static inline int w202_fgu_full_eligible(int online, int full, int healthy,
		int voltage, int battery_ua, int temp, int full_voltage, int full_current)
{
	/* Also accept the FGU's tiny negative zero offset after termination.
	 * A Full bit or high terminal voltage alone never establishes SOC.
	 */
	return online && full && healthy && voltage >= full_voltage &&
		battery_ua >= -5000 && battery_ua <= full_current && temp >= 150 && temp <= 400;
}

struct w202_fgu_full_state {
	unsigned long since, last;
	int tracking, calibrated;
};

/* Unsigned elapsed-time subtraction also handles tick wrap. A long gap
 * (including suspend) must not count as continuously observed full state.
 */
static inline int w202_fgu_confirm_full(struct w202_fgu_full_state *s, int eligible,
		unsigned long now, unsigned long duration, unsigned long max_gap)
{
	if (!eligible) {
		s->tracking = s->calibrated = 0;
		return 0;
	}
	if (!s->tracking || now - s->last > max_gap) {
		s->since = now;
		s->tracking = 1;
	}
	s->last = now;
	if (s->calibrated || now - s->since < duration)
		return 0;
	s->calibrated = 1;
	return 1;
}

#endif
