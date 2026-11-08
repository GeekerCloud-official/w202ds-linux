/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mm-modem-helpers-w202ds.h"

GRegex *
mm_w202ds_sim_urc_regex_new (void)
{
    /* The stock RIL routes ECIND group 3 to onSimStatusChanged(). Only use
     * it as a hint: confirm presence with CPIN instead of guessing the
     * meaning of every proprietary status/reason value. Do not consume
     * +CPIN replies here; the generic unlock query still needs them. */
    return g_regex_new ("\\r\\n\\+ECIND:[ \\t]*3[ \\t]*,[^\\r\\n]+\\r\\n",
                        G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}

MMW202dsSimAction
mm_w202ds_sim_check_action (gboolean                  has_sim,
                            MMModemState              state,
                            MMModemStateFailedReason  failed_reason,
                            MMModemLock               lock,
                            const GError             *error)
{
    if (error) {
        if (has_sim && g_error_matches (error, MM_MOBILE_EQUIPMENT_ERROR,
                                        MM_MOBILE_EQUIPMENT_ERROR_SIM_NOT_INSERTED))
            return MM_W202DS_SIM_ACTION_REMOVED;
        return MM_W202DS_SIM_ACTION_NONE;
    }

    if (lock == MM_MODEM_LOCK_UNKNOWN)
        return MM_W202DS_SIM_ACTION_NONE;

    if (!has_sim && state == MM_MODEM_STATE_FAILED &&
        failed_reason == MM_MODEM_STATE_FAILED_REASON_SIM_MISSING)
        return MM_W202DS_SIM_ACTION_INSERTED;

    if (has_sim && lock == MM_MODEM_LOCK_NONE)
        return MM_W202DS_SIM_ACTION_CHECK_IDENTITY;

    return MM_W202DS_SIM_ACTION_NONE;
}
