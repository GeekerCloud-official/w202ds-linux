/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_PPS_LIMITS_H
#define W202_PPS_LIMITS_H

/* Validated board ceiling, not the charger's OVP setting or source APDO max. */
#define W202_PPS_MAX_UV 9800000

static inline int w202_pps_next_uv(int voltage_uv, int minimum, int maximum, int board)
{
	int target = min(min(maximum, board), W202_PPS_MAX_UV) / 20000 * 20000;

	if (minimum <= 0)
		return -ERANGE;
	/* Many standard adapters advertise 3.3 V. Intersect that source range
	 * with this board's 5 V floor rather than rejecting the whole APDO.
	 */
	minimum = minimum < 5000000 ? 5000000 : minimum;
	if (minimum > maximum || target < minimum || voltage_uv < minimum)
		return -ERANGE;
	/* Source bounds and 20 mV granularity take precedence over ramp size. */
	return min(target, voltage_uv + 200000) / 20000 * 20000;
}

#endif
