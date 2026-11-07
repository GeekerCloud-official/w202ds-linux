// SPDX-License-Identifier: GPL-2.0-only
/* Regression: stale SOC recovery must not turn arbitrary Full/voltage into 100%. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../w202_fgu_soc.h"

int main(void)
{
	struct w202_fgu_full_state s = {};
	unsigned int soc, mode;

	for (soc = 0; soc <= 1000; soc++)
		for (mode = 0; mode < 16; mode++)
			assert(w202_fgu_decode_soc((mode << 12) | w202_fgu_encode_soc(soc)) == (int)soc);
	assert(w202_fgu_decode_soc(0x541b) == 274);
	assert(w202_fgu_decode_soc(0xa41b) == 274);
	assert(w202_fgu_decode_soc(0xffff) < 0);
	assert(w202_fgu_decode_soc(0xa05) < 0);
	assert(w202_fgu_decode_soc(0x164) < 0);
	assert(w202_fgu_decode_soc(101) < 0);
	assert(w202_fgu_full_eligible(1, 1, 1, 4352000, -1486, 294, 4330000, 120000));
	assert(!w202_fgu_full_eligible(0, 1, 1, 4352000, -1486, 294, 4330000, 120000));
	assert(!w202_fgu_full_eligible(1, 0, 1, 4352000, -1486, 294, 4330000, 120000));
	assert(!w202_fgu_full_eligible(1, 1, 0, 4352000, -1486, 294, 4330000, 120000));
	assert(!w202_fgu_full_eligible(1, 1, 1, 4200000, -1486, 294, 4330000, 120000));
	assert(!w202_fgu_full_eligible(1, 1, 1, 4352000, 500000, 294, 4330000, 120000));
	assert(!w202_fgu_full_eligible(1, 1, 1, 4352000, -500000, 294, 4330000, 120000));
	assert(!w202_fgu_full_eligible(1, 1, 1, 4352000, -1486, 100, 4330000, 120000));
	assert(!w202_fgu_full_eligible(1, 1, 1, 4352000, -1486, 450, 4330000, 120000));
	assert(!w202_fgu_confirm_full(&s, 1, 0, 30, 20));
	assert(!w202_fgu_confirm_full(&s, 1, 10, 30, 20));
	assert(!w202_fgu_confirm_full(&s, 1, 20, 30, 20));
	assert(w202_fgu_confirm_full(&s, 1, 30, 30, 20));
	assert(!w202_fgu_confirm_full(&s, 1, 40, 30, 20)); /* Once per full episode. */
	assert(!w202_fgu_confirm_full(&s, 0, 50, 30, 20));
	assert(!w202_fgu_confirm_full(&s, 1, 60, 30, 20));
	assert(!w202_fgu_confirm_full(&s, 1, 100, 30, 20)); /* Gap restarts confirmation. */
	assert(!w202_fgu_confirm_full(&s, 1, 110, 30, 20));
	assert(!w202_fgu_confirm_full(&s, 1, 120, 30, 20));
	assert(w202_fgu_confirm_full(&s, 1, 130, 30, 20));
	s = (struct w202_fgu_full_state){};
	assert(!w202_fgu_confirm_full(&s, 1, ULONG_MAX - 19, 30, 20));
	assert(!w202_fgu_confirm_full(&s, 1, ULONG_MAX - 9, 30, 20));
	assert(!w202_fgu_confirm_full(&s, 1, 0, 30, 20));
	assert(w202_fgu_confirm_full(&s, 1, 10, 30, 20));
	puts("FGU SOC: 16016 RTC roundtrips, invalid saves, full guards, gap and wrap passed");
	return 0;
}
