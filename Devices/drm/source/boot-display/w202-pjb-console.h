/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_W202_PJB_CONSOLE_H
#define _LINUX_W202_PJB_CONSOLE_H
#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_W202_PJB_CONSOLE)
bool w202_pjb_console_active(void);
/* Process context: waits for all pending framebuffer/MMIO writes to stop.
 * Clock references survive until native generic aperture takeover. */
void w202_pjb_console_release(void);
#else
static inline bool w202_pjb_console_active(void) { return false; }
static inline void w202_pjb_console_release(void) { }
#endif
#endif
