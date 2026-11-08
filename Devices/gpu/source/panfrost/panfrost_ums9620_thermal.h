/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PANFROST_UMS9620_THERMAL_H
#define PANFROST_UMS9620_THERMAL_H

/* Callers supply bool. A failed sample must never release the limit; after
 * recovery, keep the same hysteresis as an over-temperature event.
 */
static inline bool ums_thermal_cap_required(bool capped, int read_error,
					  int temp, int hot_mc, int cool_mc)
{
	return read_error || temp >= hot_mc || (capped && temp > cool_mc);
}

#endif
