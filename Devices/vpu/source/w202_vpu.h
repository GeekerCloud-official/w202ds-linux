/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_VPU_H
#define W202_VPU_H

#include <linux/types.h>

bool w202_vpu_power_ready(void);
struct device;
/* Process-context power lease, kept for the full lifetime of an MMIO owner. */
int w202_vpu_iommu_get(struct device *dev);
void w202_vpu_iommu_put(struct device *dev);
int w202_vpu_iommu_restore(struct device *dev);

#endif
