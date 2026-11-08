/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "../panfrost_ums9620_thermal.h"

int main(void)
{
	bool cap = true; /* Initial QoS must be conservative. */
	const struct {
		int error, temp;
		bool expected;
	} sequence[] = {
		{ -19, 0, true },       /* Sensor has not produced a sample. */
		{ 0, 70000, true },     /* Not cool enough to release initial cap. */
		{ 0, 65000, false },    /* Inclusive release threshold. */
		{ 0, 74999, false },    /* Below hot threshold, keep high OPPs. */
		{ 0, 75000, true },     /* Inclusive hot threshold. */
		{ 0, 70000, true },     /* Hysteresis prevents toggling. */
		{ 0, 65001, true },
		{ 0, 65000, false },
		{ -5, 0, true },        /* Read failure restores the cap. */
		{ -11, 0, true },       /* Repeated errors retain it. */
		{ 0, 70000, true },     /* Recovery also respects hysteresis. */
		{ 0, 40000, false },
	};
	unsigned int i;

	for (i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
		cap = ums_thermal_cap_required(cap, sequence[i].error,
					      sequence[i].temp, 75000, 65000);
		assert(cap == sequence[i].expected);
	}
	puts("thermal policy: 12 transitions passed");
	return 0;
}
