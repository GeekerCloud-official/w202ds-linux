// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/mutex.h>
#include "w202_panel_sync.h"
static DEFINE_MUTEX(panel_lock);
static BLOCKING_NOTIFIER_HEAD(panel_clients);
/* Touch is loaded after display initializes the panel. Late registration
 * synchronizes with the last successful transition while holding this lock.
 */
static bool panel_awake = true;
int w202_panel_register_client(struct notifier_block *nb)
{
 int ret;
 mutex_lock(&panel_lock);
 ret = blocking_notifier_chain_register(&panel_clients, nb);
 if (!ret && !panel_awake) {
  ret = notifier_to_errno(nb->notifier_call(nb, W202_PANEL_OFF, NULL));
  if (ret) blocking_notifier_chain_unregister(&panel_clients, nb);
 }
 mutex_unlock(&panel_lock);
 return ret;
}
EXPORT_SYMBOL_GPL(w202_panel_register_client);
int w202_panel_unregister_client(struct notifier_block *nb)
{
 int ret;
 mutex_lock(&panel_lock);
 ret = blocking_notifier_chain_unregister(&panel_clients, nb);
 mutex_unlock(&panel_lock);
 return ret;
}
EXPORT_SYMBOL_GPL(w202_panel_unregister_client);
int w202_panel_set_awake(bool awake)
{
 int ret;
 mutex_lock(&panel_lock);
 if (panel_awake == awake) {
  mutex_unlock(&panel_lock);
  return 0;
 }
 ret = notifier_to_errno(blocking_notifier_call_chain(&panel_clients,
          awake ? W202_PANEL_ON : W202_PANEL_OFF, NULL));
 if (!ret) panel_awake = awake;
 mutex_unlock(&panel_lock);
 return ret;
}
