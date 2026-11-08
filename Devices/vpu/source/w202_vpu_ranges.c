// SPDX-License-Identifier: GPL-2.0-only
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/sort.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#endif
#include "w202_vpu_ranges.h"

static int range_compare(const void *left, const void *right)
{
	const struct w202_vpu_range *a = left, *b = right;

	if (a->first != b->first)
		return a->first < b->first ? -1 : 1;
	return (a->last > b->last) - (a->last < b->last);
}

int w202_vpu_ranges_validate(struct w202_vpu_range *ranges, __u32 count)
{
	__u64 furthest[W202_VPU_RANGE_USES] = {};
	bool seen[W202_VPU_RANGE_USES] = {};
	__u32 i, use;

	if (!ranges || !count)
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (ranges[i].first > ranges[i].last || ranges[i].use >= W202_VPU_RANGE_USES)
			return -EINVAL;
#ifdef __KERNEL__
	sort(ranges, count, sizeof(*ranges), range_compare, NULL);
#else
	qsort(ranges, count, sizeof(*ranges), range_compare);
#endif
	for (i = 0; i < count; i++) {
		const struct w202_vpu_range *range = &ranges[i];

		for (use = 0; use < W202_VPU_RANGE_USES; use++)
			if ((use != range->use || use == W202_VPU_RANGE_OUTPUT) &&
			    seen[use] && range->first <= furthest[use])
				return -EINVAL;
		if (!seen[range->use] || range->last > furthest[range->use])
			furthest[range->use] = range->last;
		seen[range->use] = true;
	}
	return 0;
}
