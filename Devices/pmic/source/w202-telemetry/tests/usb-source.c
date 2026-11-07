// SPDX-License-Identifier: GPL-2.0-only
/* Host test: source capability must never be inferred from stale/invalid CC. */
#include <assert.h>
#include <stdio.h>
#include "../w202_usb_source.h"

int main(void)
{
	unsigned int state, rp;

	assert(w202_usb_source_current(0, 1, 0x82, 0x100) == 0);
	assert(w202_usb_source_current(4, 0, 0x82, 0x100) == 500000);
	assert(w202_usb_source_current(4, 1, 0x82, 0) == 500000);
	assert(w202_usb_source_current(4, 1, 0x82, 0x100) == 3000000);
	assert(w202_usb_source_current(4, 1, 0x42, 0x100) == 1500000);
	assert(w202_usb_source_current(4, 1, 2, 0x100) == 500000);
	assert(w202_usb_source_current(4, 1, 0xc2, 0x100) == 500000);
	assert(w202_usb_source_current(0x44, 0, 0, 0) == 500000); /* BC not done */
	assert(w202_usb_source_current(0x844, 0, 0, 0) == 1500000); /* DCP */
	assert(w202_usb_source_current(0x824, 0, 0, 0) == 1500000); /* CDP */
	assert(w202_usb_source_current(0x884, 0, 0, 0) == 500000); /* SDP */
	assert(w202_usb_source_current(0x8e4, 0, 0, 0) == 500000); /* ambiguous */
	for (state = 0; state < 32; state++) {
		if (state == 2)
			continue;
		for (rp = 0; rp < 4; rp++)
			assert(w202_usb_source_current(4, 1, state | (rp << 6), 0x100) == 500000);
	}
	/* A contract can be lower than Rp. PD can also grant more than default
	 * Rp; a non-PD provider cannot. Clamp an invalid oversized budget.
	 */
	assert(w202_usb_sink_budget(3000000, 1000000, 1) == 1000000);
	assert(w202_usb_sink_budget(500000, 3000000, 1) == 3000000);
	assert(w202_usb_sink_budget(500000, 3000000, 0) == 500000);
	assert(w202_usb_sink_budget(3000000, 500000, 0) == 500000);
	assert(w202_usb_sink_budget(3000000, 0, 1) == 0);
	assert(w202_usb_sink_budget(3000000, 6000000, 1) == 3000000);
	puts("USB source: presence, completion, 124 stale states and TCPM budgets passed");
	return 0;
}
