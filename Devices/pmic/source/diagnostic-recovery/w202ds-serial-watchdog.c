// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/capability.h>
void w202ds_emergency_fastboot(void);
static void w202ds_serial_timeout(struct work_struct *work)
{
    pr_emerg("[W202DS-M9] kernel timeout; returning to fastboot\n");
    w202ds_emergency_fastboot();
}
static DECLARE_DELAYED_WORK(w202ds_serial_recovery, w202ds_serial_timeout);
static long w202ds_diag_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if (cmd != _IO('W', 3) && cmd != _IO('W', 4)) return -ENOTTY;
    if (!capable(CAP_SYS_BOOT)) return -EPERM;
    cancel_delayed_work_sync(&w202ds_serial_recovery);
    if (cmd == _IO('W', 4)) {
        mod_delayed_work(system_unbound_wq, &w202ds_serial_recovery, 30 * HZ);
        pr_info("[W202DS-M9] diagnostic recovery rearmed for 30 seconds\n");
        return 0;
    }
    pr_info("[W202DS-M9] healthy USB/display; timed recovery cancelled\n");
    return 0;
}
static const struct file_operations diag_fops = {
    .owner = THIS_MODULE, .unlocked_ioctl = w202ds_diag_ioctl,
};
static struct miscdevice diag_dev = {
    .minor = MISC_DYNAMIC_MINOR, .name = "w202diagctl", .fops = &diag_fops, .mode = 0600,
};
static int __init w202ds_serial_arm_recovery(void)
{
    if (!CONFIG_W202DS_DIAG_BOOT_TIMEOUT)
        return 0;
    queue_delayed_work(system_unbound_wq, &w202ds_serial_recovery, CONFIG_W202DS_DIAG_BOOT_TIMEOUT * HZ);
    pr_emerg("[W202DS-M9] kernel recovery armed for %u seconds\n", CONFIG_W202DS_DIAG_BOOT_TIMEOUT);
    return 0;
}
subsys_initcall(w202ds_serial_arm_recovery);
static int __init w202ds_diag_register(void) { return misc_register(&diag_dev); }
late_initcall(w202ds_diag_register);
