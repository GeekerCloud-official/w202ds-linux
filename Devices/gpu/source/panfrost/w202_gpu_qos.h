/* SPDX-License-Identifier: GPL-2.0-only */
/* Qogirn6Pro NIC400 settings from vendor mali_kbase_gpu_sys_qogirn6pro_qos.h.
 * Both GPU bus master ports use the same values. No undocumented tuning.
 */
#ifndef W202_GPU_QOS_H
#define W202_GPU_QOS_H
struct ums_qos_reg { u32 offset, mask, value; };
static const struct ums_qos_reg ums_nic_qos[] = {
 {0x00,0x00000001,0x00000000},
 {0x04,0xffffffff,0x0a080808},
 {0x08,0x3f3f3f3f,0x10101010},
 {0x0c,0x3f3fffff,0x20100a0a},
 {0x40,0x00000003,0x00000000},
 {0x44,0xffffffff,0x00000000},
 {0x48,0x073fffff,0x00000000},
 {0x4c,0xffffffff,0x00000000},
 {0x50,0x073fffff,0x00000000},
 {0x60,0x80000003,0x00000003},
 {0x64,0x3fff3fff,0x06660666},
 {0x68,0x00000701,0x00000001},
};
#endif
