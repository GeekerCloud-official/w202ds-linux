/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#define min(a, b) ((a) < (b) ? (a) : (b))
#include "../pps-limits.h"

int main(void)
{
	int board, maximum, minimum, current, next, count = 0;

	/* A smaller adapter, a smaller configured board, and a stronger source. */
	assert(w202_pps_next_uv(8900000, 5000000, 9000000, 12000000) == 9000000);
	assert(w202_pps_next_uv(9000000, 5000000, 11000000, 9000000) == 9000000);
	assert(w202_pps_next_uv(9800000, 5000000, 11000000, 12000000) == 9800000);
	assert(w202_pps_next_uv(9000000, 10000000, 11000000, 12000000) == -ERANGE);
	assert(w202_pps_next_uv(5000000, 5000000, 4900000, 12000000) == -ERANGE);
	assert(w202_pps_next_uv(5000000, 3300000, 11000000, 12000000) == 5200000);
	assert(w202_pps_next_uv(3300000, 3300000, 11000000, 12000000) == -ERANGE);
	assert(w202_pps_next_uv(5000000, 0, 11000000, 12000000) == -ERANGE);
	/* Sweep source caps (including non-step boundaries) and ramp to the
	 * maximum without ever crossing the negotiated or board limit.
	 */
	for (minimum = 3300000; minimum <= 5000000; minimum += 1700000)
	for (board = 9000000; board <= 12000000; board += 3000000)
		for (maximum = 5000000; maximum <= 15000000; maximum += 10001) {
			current = 5000000;
			do {
				next = w202_pps_next_uv(current, minimum, maximum, board);
				assert(next >= current && next <= current + 200000);
				assert(next <= maximum && next <= board && next <= W202_PPS_MAX_UV);
				assert(next % 20000 == 0);
				++count;
				if (next == current)
					break;
				current = next;
			} while (1);
			assert(current == min(min(maximum, board), W202_PPS_MAX_UV) / 20000 * 20000);
		}
	printf("PPS source/board ramp boundaries: %d decisions passed\n", count);
	return 0;
}
