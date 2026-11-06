/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PANFROST_UMS9620_H
#define PANFROST_UMS9620_H
#include <linux/of.h>
#include "panfrost_device.h"
static inline bool panfrost_is_ums9620(struct panfrost_device *pfdev)
{
	return of_device_is_compatible(pfdev->dev->of_node, "sprd,mali-natt");
}
int panfrost_ums9620_init(struct panfrost_device *pfdev);
int panfrost_ums9620_runtime_suspend(struct panfrost_device *pfdev);
int panfrost_ums9620_runtime_resume(struct panfrost_device *pfdev);
#endif
