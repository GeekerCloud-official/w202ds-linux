/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mm-modem-helpers-w202ds.h"

static void
test_notifications (void)
{
    g_autoptr(GRegex) regex = mm_w202ds_sim_urc_regex_new ();
    const gchar *events[] = {
        "\r\n+ECIND: 3,1,1\r\n",
        "\r\n+ECIND:3,6,0\r\n",
        "\r\n+ECIND: 3,2,22,0,0,1\r\n",
    };
    const gchar *other[] = {
        "\r\n+ECIND: 30,1,1\r\n",
        "\r\n+ECIND: 1,3,1\r\n",
        "\r\n+ECIND: 3,\r\n",
        "\r\n+CPIN: READY\r\n\r\nOK\r\n",
        "\r\n+CEREG: 0\r\n",
        "\r\n+CME ERROR: 10\r\n",
        "\r\n+ECIND: 3,6",
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (events); i++)
        g_assert_true (g_regex_match (regex, events[i], 0, NULL));
    for (i = 0; i < G_N_ELEMENTS (other); i++)
        g_assert_false (g_regex_match (regex, other[i], 0, NULL));
}

static void
test_presence (void)
{
    g_autoptr(GError) missing = g_error_new_literal (MM_MOBILE_EQUIPMENT_ERROR,
        MM_MOBILE_EQUIPMENT_ERROR_SIM_NOT_INSERTED, "SIM not inserted");
    g_autoptr(GError) timeout = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "timeout");
    g_autoptr(GError) sim_failure = g_error_new_literal (MM_MOBILE_EQUIPMENT_ERROR,
        MM_MOBILE_EQUIPMENT_ERROR_SIM_FAILURE, "SIM failure");

    /* Removal tears down a stale object; repeated absent indications do not
     * reprobe an already absent card. A lost registration alone is not removal. */
    g_assert_cmpint (mm_w202ds_sim_check_action (TRUE, MM_MODEM_STATE_REGISTERED,
        MM_MODEM_STATE_FAILED_REASON_NONE, MM_MODEM_LOCK_UNKNOWN, missing), ==,
        MM_W202DS_SIM_ACTION_REMOVED);
    g_assert_cmpint (mm_w202ds_sim_check_action (FALSE, MM_MODEM_STATE_FAILED,
        MM_MODEM_STATE_FAILED_REASON_SIM_MISSING, MM_MODEM_LOCK_UNKNOWN, missing), ==,
        MM_W202DS_SIM_ACTION_NONE);
    g_assert_cmpint (mm_w202ds_sim_check_action (TRUE, MM_MODEM_STATE_ENABLED,
        MM_MODEM_STATE_FAILED_REASON_NONE, MM_MODEM_LOCK_UNKNOWN, timeout), ==,
        MM_W202DS_SIM_ACTION_NONE);
    g_assert_cmpint (mm_w202ds_sim_check_action (TRUE, MM_MODEM_STATE_REGISTERED,
        MM_MODEM_STATE_FAILED_REASON_NONE, MM_MODEM_LOCK_UNKNOWN, sim_failure), ==,
        MM_W202DS_SIM_ACTION_NONE);
    g_assert_cmpint (mm_w202ds_sim_check_action (TRUE, MM_MODEM_STATE_REGISTERED,
        MM_MODEM_STATE_FAILED_REASON_NONE, MM_MODEM_LOCK_UNKNOWN, NULL), ==,
        MM_W202DS_SIM_ACTION_NONE);

    /* Both unlocked and PIN/PUK-locked inserted cards must leave SIM_MISSING.
     * Other startup failures must not become a reprobe loop. */
    g_assert_cmpint (mm_w202ds_sim_check_action (FALSE, MM_MODEM_STATE_FAILED,
        MM_MODEM_STATE_FAILED_REASON_SIM_MISSING, MM_MODEM_LOCK_NONE, NULL), ==,
        MM_W202DS_SIM_ACTION_INSERTED);
    g_assert_cmpint (mm_w202ds_sim_check_action (FALSE, MM_MODEM_STATE_FAILED,
        MM_MODEM_STATE_FAILED_REASON_SIM_MISSING, MM_MODEM_LOCK_SIM_PIN, NULL), ==,
        MM_W202DS_SIM_ACTION_INSERTED);
    g_assert_cmpint (mm_w202ds_sim_check_action (FALSE, MM_MODEM_STATE_FAILED,
        MM_MODEM_STATE_FAILED_REASON_SIM_MISSING, MM_MODEM_LOCK_SIM_PUK, NULL), ==,
        MM_W202DS_SIM_ACTION_INSERTED);
    g_assert_cmpint (mm_w202ds_sim_check_action (FALSE, MM_MODEM_STATE_INITIALIZING,
        MM_MODEM_STATE_FAILED_REASON_NONE, MM_MODEM_LOCK_NONE, NULL), ==,
        MM_W202DS_SIM_ACTION_NONE);
    g_assert_cmpint (mm_w202ds_sim_check_action (FALSE, MM_MODEM_STATE_FAILED,
        MM_MODEM_STATE_FAILED_REASON_UNKNOWN, MM_MODEM_LOCK_NONE, NULL), ==,
        MM_W202DS_SIM_ACTION_NONE);
    g_assert_cmpint (mm_w202ds_sim_check_action (TRUE, MM_MODEM_STATE_REGISTERED,
        MM_MODEM_STATE_FAILED_REASON_NONE, MM_MODEM_LOCK_NONE, NULL), ==,
        MM_W202DS_SIM_ACTION_CHECK_IDENTITY);
    g_assert_cmpint (mm_w202ds_sim_check_action (TRUE, MM_MODEM_STATE_LOCKED,
        MM_MODEM_STATE_FAILED_REASON_NONE, MM_MODEM_LOCK_SIM_PIN, NULL), ==,
        MM_W202DS_SIM_ACTION_NONE);
}

int main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/w202ds/sim/notifications", test_notifications);
    g_test_add_func ("/w202ds/sim/presence", test_presence);
    return g_test_run ();
}
