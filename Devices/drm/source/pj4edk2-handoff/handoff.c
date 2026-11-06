// SPDX-License-Identifier: GPL-2.0-only
/* Standalone userspace-controlled release of the optional early console.
 * No DRM imports, exports, callbacks, framebuffer allocation or display MMIO.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

/* Optional early-console ABI, resolved and pinned independently of DRM. */
extern bool w202_pjb_console_active(void);
extern void w202_pjb_console_release(void);
static bool (*console_active)(void);
static void (*console_release)(void);
static DEFINE_MUTEX(release_lock);
static bool release_requested;
static unsigned int releases;
module_param(releases, uint, 0444);

static void release_console(void)
{
	if (release_requested && console_active && console_active()) {
		console_release();
		releases++;
	}
}

static int available_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", !!console_active);
}

static int active_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", console_active && console_active());
}

static int release_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(release_requested));
}

static int release_set(const char *value, const struct kernel_param *kp)
{
	bool release;
	int ret = kstrtobool(value, &release);

	if (ret)
		return ret;
	mutex_lock(&release_lock);
	if (!release && release_requested) {
		mutex_unlock(&release_lock);
		return -EPERM;
	}
	if (release && !release_requested) {
		WRITE_ONCE(release_requested, true);
		release_console();
	}
	mutex_unlock(&release_lock);
	return 0;
}

static const struct kernel_param_ops available_ops = { .get = available_get };
static const struct kernel_param_ops active_ops = { .get = active_get };
static const struct kernel_param_ops release_ops = {
	.set = release_set, .get = release_get,
};
module_param_cb(console_available, &available_ops, NULL, 0444);
module_param_cb(console_active, &active_ops, NULL, 0444);
module_param_cb(release, &release_ops, NULL, 0644);
MODULE_PARM_DESC(release, "Write 1 to drain and stop the early writer; clock ownership is independent (one-way)");

static int __init pj4edk2_handoff_init(void)
{
	console_active = symbol_get(w202_pjb_console_active);
	console_release = symbol_get(w202_pjb_console_release);
	if (!console_active || !console_release) {
		if (console_active)
			symbol_put(w202_pjb_console_active);
		if (console_release)
			symbol_put(w202_pjb_console_release);
		console_active = NULL;
		console_release = NULL;
	}
	mutex_lock(&release_lock);
	release_console();
	mutex_unlock(&release_lock);
	pr_info("pj4edk2-handoff: early_console=%u release=%u\n",
		!!console_active, release_requested);
	return 0;
}

static void __exit pj4edk2_handoff_exit(void)
{
	/* Unload itself does not release ownership or change hardware. */
	if (console_release)
		symbol_put(w202_pjb_console_release);
	if (console_active)
		symbol_put(w202_pjb_console_active);
}
module_init(pj4edk2_handoff_init);
module_exit(pj4edk2_handoff_exit);
MODULE_DESCRIPTION("pj4edk2 independent early-display release control");
MODULE_AUTHOR("W202DS project");
MODULE_LICENSE("GPL");
