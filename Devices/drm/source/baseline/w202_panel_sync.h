/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_PANEL_SYNC_H
#define W202_PANEL_SYNC_H
#include <linux/notifier.h>
/* Blocking callbacks: OFF completes before DCS sleep; ON follows scanout.
 * This is retained-power DPMS only, not system suspend or rail poweroff.
 * Callbacks must not call back into display/modeset or take display locks.
 */
#define W202_PANEL_OFF 0
#define W202_PANEL_ON  1
int w202_panel_register_client(struct notifier_block *nb);
int w202_panel_unregister_client(struct notifier_block *nb);
int w202_panel_set_awake(bool awake);
#endif
