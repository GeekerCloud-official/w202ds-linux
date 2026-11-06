/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_R6P0_M13_H
#define W202_R6P0_M13_H

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_mode.h>
#include <drm/drm_simple_kms_helper.h>

struct w202_dsi_state;
struct w202_boot_console;
struct backlight_device;

struct w202_dpu {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct drm_display_mode modes[2];
	struct device *dev;
	struct backlight_device *backlight;
	void __iomem *regs;
	struct clk *clks[5];
	struct completion updated, stopped, configured;
	struct mutex hw_lock;
	struct mutex activation_lock;
	struct w202_dsi_state *dsi;
	struct w202_boot_console *boot_console;
	u32 irq_mask;
	u64 vsyncs, updates, commits;
	u64 errors;
	u64 latch_last_us, latch_max_us, latch_timeouts;
	u32 last_error_status;
	bool hw_initialized;
	bool scanout_allowed;
	bool display_ready;
	bool fbdev_started;
	bool first_frame_visible;
	bool irq_requested;
	unsigned int clocks_enabled;
	int irq;
};

int w202_dsi_start(struct w202_dpu *d);
int w202_dsi_claim_clocks(struct w202_dpu *d);
int w202_dpu_read_mode(struct w202_dpu *d);
int w202_dpu_hw_init(struct w202_dpu *d);
int w202_dsi_set_mode(struct w202_dpu *d, const struct drm_display_mode *mode);
int w202_dsi_set_power(struct w202_dpu *d, bool on);
ssize_t w202_dsi_status(struct w202_dpu *d, char *buf);
int w202_boot_console_prepare(struct w202_dpu *d);
void w202_boot_console_capture(struct w202_dpu *d);
int w202_boot_console_start(struct w202_dpu *d);
void w202_boot_console_stop(struct w202_dpu *d);
bool w202_boot_console_active(struct w202_dpu *d);
ssize_t w202_boot_console_status(struct w202_dpu *d, char *buf);

#endif
