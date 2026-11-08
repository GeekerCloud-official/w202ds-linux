// SPDX-License-Identifier: GPL-2.0-only
/* Compare the resource layer's physical-range checker against an exhaustive
 * pairwise oracle. No decoder, DRM or device node is opened by this test. */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../source/w202_vpu_ranges.h"

static unsigned int tests;
static uint64_t random_state = UINT64_C(0x202d5a1b7);
static uint64_t next_random(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return random_state;
}

static bool oracle(const struct w202_vpu_range *r, unsigned int count)
{
	if (!count)
		return false;
	for (unsigned int i = 0; i < count; i++) {
		if (r[i].first > r[i].last || r[i].use >= W202_VPU_RANGE_USES)
			return false;
		for (unsigned int j = 0; j < i; j++) {
			bool conflict = r[i].use != r[j].use || r[i].use == W202_VPU_RANGE_OUTPUT;
			bool overlap = r[i].first <= r[j].last && r[j].first <= r[i].last;

			if (conflict && overlap)
				return false;
		}
	}
	return true;
}

static void test(struct w202_vpu_range *ranges, unsigned int count)
{
	struct w202_vpu_range sorted[128];
	bool expected = oracle(ranges, count);

	memcpy(sorted, ranges, count * sizeof(*ranges));
	int ret = w202_vpu_ranges_validate(sorted, count);
	if (ret != (expected ? 0 : -EINVAL)) {
		fprintf(stderr, "range mismatch case=%u count=%u expected=%d ret=%d\n",
			tests, count, expected, ret);
		for (unsigned int i = 0; i < count; i++)
			fprintf(stderr, "range=%u first=%" PRIu64 " last=%" PRIu64 " use=%u\n",
				i, (uint64_t)ranges[i].first, (uint64_t)ranges[i].last, ranges[i].use);
		exit(1);
	}
	tests++;
}

int main(void)
{
	struct w202_vpu_range r[128] = {
		{0, 4095, W202_VPU_RANGE_INPUT},
		{4096, 8191, W202_VPU_RANGE_OUTPUT},
		{8192, 12287, W202_VPU_RANGE_REFERENCE},
		{8192, 12287, W202_VPU_RANGE_REFERENCE},
	};
	if (w202_vpu_ranges_validate(NULL, 1) != -EINVAL)
		return 2;
	test(r, 0);
	test(r, 4); /* adjacent ranges and two exports of a read-only reference */
	r[1].first = 4095;
	test(r, 4); /* one overlapping byte */
	r[1].first = 4096;
	r[3] = r[1];
	test(r, 4); /* distinct output exports aliasing the same storage */
	r[3] = (struct w202_vpu_range){1, 2, W202_VPU_RANGE_REFERENCE};
	test(r, 4); /* input contains a reference range */
	r[0] = (struct w202_vpu_range){UINT64_MAX - 1, UINT64_MAX, W202_VPU_RANGE_INPUT};
	r[1] = (struct w202_vpu_range){0, 0, W202_VPU_RANGE_OUTPUT};
	test(r, 2);
	r[1] = (struct w202_vpu_range){UINT64_MAX, UINT64_MAX, W202_VPU_RANGE_OUTPUT};
	test(r, 2);
	r[0].first = 9; r[0].last = 8;
	test(r, 2);
	r[0] = (struct w202_vpu_range){0, 0, W202_VPU_RANGE_USES};
	test(r, 1);
	for (unsigned int trial = 0; trial < 20000; trial++) {
		unsigned int count = next_random() % 128 + 1;
		uint64_t base = next_random() & UINT64_C(0xffffffffffff0000);

		for (unsigned int i = 0; i < count; i++) {
			r[i].first = base + (next_random() & 0x1fff);
			r[i].last = r[i].first + (next_random() & 0xff);
			r[i].use = next_random() % W202_VPU_RANGE_USES;
		}
		test(r, count);
		/* The same addresses spread apart must pass in arbitrary order. */
		for (unsigned int i = 0; i < count; i++) {
			r[i].first = base + i * 256;
			r[i].last = r[i].first + (next_random() & 0x7f);
		}
		for (unsigned int i = 1; i < count; i++) {
			unsigned int other = next_random() % (i + 1);
			struct w202_vpu_range swap = r[i];
			r[i] = r[other];
			r[other] = swap;
		}
		test(r, count);
	}
	printf("{\"range_cases\":%u,\"oracle\":\"exhaustive pairwise\",\"pass\":true}\n", tests);
	return 0;
}
