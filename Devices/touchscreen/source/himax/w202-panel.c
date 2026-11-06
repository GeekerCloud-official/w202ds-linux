// SPDX-License-Identifier: GPL-2.0-only
/* Retained-firmware coordination for the shared HX83102J display/touch IC.
 * No reset, firmware reload or exception-recovery suppression on DPMS.
 */
#include <linux/interrupt.h>
#include <linux/module.h>
#include "himax_platform.h"
#include "himax_common.h"
#include "himax_ic_core.h"
#include "w202_panel_sync.h"
#include "w202-panel.h"
extern void himax_report_all_leave_event(struct himax_ts_data *ts);
static DEFINE_MUTEX(touch_panel_lock);
static bool registered, quiet;
static u8 fw_version[4];
static u64 suspends, resumes;
static int last_error;
static atomic64_t valid_packets = ATOMIC64_INIT(0);
void w202_himax_valid_packet(void) { atomic64_inc(&valid_packets); }
/* The vendor void helper has a weak retry condition. Require successful
 * bus transfers AND exact readback for both suspend/resume notifications. */
static int firmware_notify(bool asleep)
{
 u8 want[4], got[4];
 int i, ret;
 hx_parse_assign_cmd(asleep ? hx_s_ic_setup._func_en :
              hx_s_ic_setup._func_dis, want, sizeof(want));
 for (i = 0; i < 20; i++) {
  ret = hx_s_core_fp._register_write(hx_s_ic_setup._func_ap_notify_fw_sus,
              want, sizeof(want));
  if (ret == NO_ERR) {
   usleep_range(2000, 3000);
   ret = hx_s_core_fp._register_read(hx_s_ic_setup._func_ap_notify_fw_sus,
              got, sizeof(got));
   if (ret == NO_ERR && !memcmp(want, got, sizeof(got))) return 0;
  }
  usleep_range(5000, 6000);
 }
 return -EIO;
}
static int firmware_retained(void)
{
 u8 version[4], reload[4], expected[4];
 int ret;
 hx_parse_assign_cmd(DATA_CHK_FW_RELOAD2, expected, sizeof(expected));
 ret = hx_s_core_fp._register_read(hx_s_ic_setup._addr_fw_ver,
             version, sizeof(version));
 if (ret != NO_ERR || memcmp(version, fw_version, sizeof(version))) return -EIO;
 ret = hx_s_core_fp._register_read(hx_s_ic_setup._addr_chk_fw_reload2,
             reload, sizeof(reload));
 if (ret != NO_ERR || memcmp(reload, expected, sizeof(reload))) return -EIO;
 return 0;
}
static int panel_event(struct notifier_block *nb, unsigned long event, void *data)
{
 struct himax_ts_data *ts = hx_s_ts;
 int ret = 0;
 mutex_lock(&touch_panel_lock);
 if (event == W202_PANEL_OFF && !quiet) {
  if (!ts->initialized || !ts->use_irq || ts->in_self_test ||
      (hx_s_debug_data && hx_s_debug_data->flash_dump_going)) {
   ret = -EBUSY;
   goto out;
  }
  himax_int_enable(0);
  synchronize_irq(ts->hx_irq);
  ret = firmware_notify(true);
  if (ret) {
   /* Display remains awake when PREPARE_OFF is rejected. */
   firmware_notify(false);
   himax_int_enable(1);
   goto out;
  }
  ts->suspended = true;
  atomic_set(&ts->suspend_mode, 1);
  ts->pre_finger_mask = 0;
  himax_report_all_leave_event(ts);
  quiet = true;
  suspends++;
 } else if (event == W202_PANEL_ON && quiet) {
  ret = firmware_notify(false);
  if (!ret) ret = firmware_retained();
  if (ret) goto out; /* Keep IRQ disabled on a failed wake; expose error. */
  ts->suspended = false;
  atomic_set(&ts->suspend_mode, 0);
  ts->diag_cmd = 0;
  himax_report_all_leave_event(ts);
  quiet = false;
  resumes++;
  himax_int_enable(1);
 }
out:
 last_error = ret;
 dev_info(ts->dev, "W202 retained panel %s quiet=%u error=%d cycles=%llu/%llu\n",
       event == W202_PANEL_ON ? "wake" : "sleep", quiet, ret, suspends, resumes);
 mutex_unlock(&touch_panel_lock);
 return notifier_from_errno(ret);
}
static struct notifier_block panel_nb = { .notifier_call = panel_event };
static ssize_t w202_panel_state_show(struct device *dev,
       struct device_attribute *attr, char *buf)
{
 ssize_t ret;
 mutex_lock(&touch_panel_lock);
 ret = sysfs_emit(buf, "registered=%u quiet=%u error=%d suspends=%llu resumes=%llu irq=%d packets=%lld fw=%02x%02x%02x%02x\n",
   registered, quiet, last_error, suspends, resumes,
   atomic_read(&hx_s_ts->irq_state), atomic64_read(&valid_packets),
   fw_version[0], fw_version[1], fw_version[2], fw_version[3]);
 mutex_unlock(&touch_panel_lock);
 return ret;
}
static DEVICE_ATTR_RO(w202_panel_state);
int w202_himax_panel_register(void)
{
 int ret;
 ret = hx_s_core_fp._register_read(hx_s_ic_setup._addr_fw_ver,
            fw_version, sizeof(fw_version));
 if (ret != NO_ERR || !(fw_version[1] | fw_version[2])) return -EIO;
 ret = firmware_retained();
 if (ret) return ret;
 ret = device_create_file(hx_s_ts->dev, &dev_attr_w202_panel_state);
 if (ret) return ret;
 ret = w202_panel_register_client(&panel_nb);
 if (ret) {
  device_remove_file(hx_s_ts->dev, &dev_attr_w202_panel_state);
  return ret;
 }
 mutex_lock(&touch_panel_lock);
 registered = true;
 /* Boot worker must not enable IRQ over a synchronized standby. */
 if (!quiet) himax_int_enable(1);
 mutex_unlock(&touch_panel_lock);
 return 0;
}
void w202_himax_panel_unregister(void)
{
 if (!registered) return;
 w202_panel_unregister_client(&panel_nb);
 device_remove_file(hx_s_ts->dev, &dev_attr_w202_panel_state);
 registered = false;
}
