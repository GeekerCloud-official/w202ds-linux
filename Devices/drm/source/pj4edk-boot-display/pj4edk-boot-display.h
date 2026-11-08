/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PJ4EDK_BOOT_DISPLAY_H
#define _LINUX_PJ4EDK_BOOT_DISPLAY_H
#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_PJ4EDK_BOOT_DISPLAY)
bool pj4edk_boot_display_console_active(void);
/* Process context: waits for all pending framebuffer/MMIO writes to stop.
 * Clock references survive until native generic aperture takeover. */
void pj4edk_boot_display_console_release(void);
#else
static inline bool pj4edk_boot_display_console_active(void) { return false; }
static inline void pj4edk_boot_display_console_release(void) { }
#endif
#endif
