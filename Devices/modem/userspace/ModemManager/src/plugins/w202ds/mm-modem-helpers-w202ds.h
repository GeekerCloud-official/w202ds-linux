/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MM_MODEM_HELPERS_W202DS_H
#define MM_MODEM_HELPERS_W202DS_H

#include <libmm-glib.h>

typedef enum {
    MM_W202DS_SIM_ACTION_NONE,
    MM_W202DS_SIM_ACTION_REMOVED,
    MM_W202DS_SIM_ACTION_INSERTED,
    MM_W202DS_SIM_ACTION_CHECK_IDENTITY,
} MMW202dsSimAction;

GRegex *mm_w202ds_sim_urc_regex_new (void);
MMW202dsSimAction mm_w202ds_sim_check_action (gboolean                  has_sim,
                                             MMModemState              state,
                                             MMModemStateFailedReason  failed_reason,
                                             MMModemLock               lock,
                                             const GError             *error);

#endif
