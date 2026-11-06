/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef W202_VPU_BUFFER_UAPI_H
#define W202_VPU_BUFFER_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>

/* Same 32-byte arm64 layout/command numbers as the vendor iommu_map_data.
 * FREE identifies an import by this file's IOVA+size; fd may already be closed.
 * The cache flag never bypasses the standard DMA-BUF/DMA-API path.
 */
struct w202_vpu_iova {
	__s32 fd;
	__u32 reserved0;
	__aligned_u64 size;
	__aligned_u64 iova_addr;
	__u8 need_cache_sync;
	__u8 reserved[7];
};
#define W202_VPU_GET_IOVA _IOWR('m', 11, struct w202_vpu_iova)
#define W202_VPU_FREE_IOVA _IOW('m', 12, struct w202_vpu_iova)
#endif
