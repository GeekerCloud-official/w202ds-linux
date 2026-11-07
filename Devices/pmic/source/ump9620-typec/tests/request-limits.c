// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdio.h>
#include "../request-limits.h"

int main(void)
{
	/* Independent wire vectors: fixed 5V/2.4A,9V/2A,12V/1.5A;
	 * PPS 5-11V/3A, as announced by the attached source.
	 */
	unsigned int caps[] = { 0x000190f0, 0x0002d0c8, 0x0003c096, 0xc0dc323c };
	unsigned int mv, ma, tested = 0;

	assert(w202_pd_request_allowed(caps, 4, 0x20025896, 9000, 1500, 0));
	assert(!w202_pd_request_allowed(caps, 4, 0x30025896, 9000, 1500, 1));
	assert(!w202_pd_request_allowed(caps, 4, 0x00025896, 9000, 1500, 1));
	assert(!w202_pd_request_allowed(caps, 4, 0x50025896, 9000, 1500, 1));
	assert(!w202_pd_request_allowed(caps, 8, 0x20025896, 9000, 1500, 1));
	assert(!w202_pd_request_allowed(caps, 0, 0x20025896, 9000, 1500, 1));
	assert(!w202_pd_request_allowed(caps, 4, 0x28025896, 9000, 1500, 1));
	/* 9V/1.5A PPS: voltage/current bounds and PPS opt-in are independent. */
	assert(w202_pd_request_allowed(caps, 4, 0x4003841e, 9000, 1500, 1));
	assert(!w202_pd_request_allowed(caps, 4, 0x4003841e, 9000, 1500, 0));
	for (mv = 4000; mv <= 12000; mv += 20) {
		for (ma = 0; ma <= 3500; ma += 50) {
			unsigned int rdo = 0x40000000 | ((mv / 20) << 9) | (ma / 50);
			int expected = mv >= 5000 && mv <= 9000 && ma > 0 && ma <= 1500;

			assert(w202_pd_request_allowed(caps, 4, rdo, 9000, 1500, 1) == expected);
			tested++;
		}
	}
	/* A narrower/new source, non-PPS APDO and malformed fixed budget. */
	caps[3] = 0xc08c3c14; /* 6-7V / 1A */
	assert(w202_pd_request_allowed(caps, 4, 0x40028a14, 9000, 1500, 1)); /*6.5V*/
	assert(!w202_pd_request_allowed(caps, 4, 0x4003841e, 9000, 1500, 1));
	caps[3] |= 1U << 28;
	assert(!w202_pd_request_allowed(caps, 4, 0x40028a14, 9000, 1500, 1));
	assert(!w202_pd_request_allowed(caps, 4, 0x200258c8, 9000, 1500, 1));
	assert(!w202_pd_request_allowed(caps, 4, 0x20025864, 9000, 1500, 1));
	assert(w202_pd_request_allowed(caps, 4, 0x200320c8, 9000, 3000, 0)); /*9V/2A*/
	assert(!w202_pd_request_allowed(caps, 4, 0x2003e8fa, 9000, 3000, 0)); /*source only2A*/
	assert(w202_pd_request_allowed(caps, 4, 0x200320c8, 12000, 3000, 0));
	assert(w202_pd_request_allowed(caps, 4, 0x30025896, 12000, 3000, 0)); /*12V source1.5A*/
	caps[1] = 0x0002d12c; /* A different source's fixed9V/3A must respect stock18W. */
	assert(!w202_pd_request_allowed(caps, 4, 0x2004b12c, 9000, 3000, 0));
	caps[3] = 0xc0dc323c;
	assert(w202_pd_request_allowed(caps, 4, 0x4003843c, 9000, 3000, 1)); /*3A*/
	assert(!w202_pd_request_allowed(caps, 4, 0x4003843d, 9000, 3000, 1)); /*3.05A*/
	for (mv = 4000; mv <= 12000; mv += 20) {
		for (ma = 0; ma <= 3500; ma += 50) {
			unsigned int rdo = 0x40000000 | ((mv / 20) << 9) | (ma / 50);
			int expected = mv >= 5000 && mv <= 11000 && ma > 0 && ma <= 3000;

			assert(w202_pd_request_allowed(caps, 4, rdo, 11000, 3000, 1) == expected);
			tested++;
		}
	}
	printf("PD Request limits: %u PPS boundary cases plus fixed/source/opt-in vectors passed\n", tested);
	return 0;
}
