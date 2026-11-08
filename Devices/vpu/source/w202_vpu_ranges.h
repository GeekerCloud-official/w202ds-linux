/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_VPU_RANGES_H
#define W202_VPU_RANGES_H
#include <linux/types.h>

enum w202_vpu_range_use {
	W202_VPU_RANGE_INPUT,
	W202_VPU_RANGE_OUTPUT,
	W202_VPU_RANGE_REFERENCE,
	W202_VPU_RANGE_USES,
};

/* Inclusive physical byte ranges, independent of DMA-BUF/export identities. */
struct w202_vpu_range {
	__u64 first, last;
	__u32 use;
};

/* Sorts the array. References may share storage with each other, but the
 * output must be disjoint from every other range, including itself. Input
 * storage must also be disjoint from references: BSM padding is CPU-written.
 */
int w202_vpu_ranges_validate(struct w202_vpu_range *ranges, __u32 count);
#endif
