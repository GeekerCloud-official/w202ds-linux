/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "mm-broadband-w202ds.h"
#include "mm-base-modem-at.h"
#include "mm-modem-helpers.h"
#include "mm-log-object.h"

typedef struct { MMBroadbandBearer parent; } MMBroadbandBearerW202ds;
typedef struct { MMBroadbandBearerClass parent; } MMBroadbandBearerW202dsClass;
G_DEFINE_TYPE (MMBroadbandBearerW202ds, mm_broadband_bearer_w202ds, MM_TYPE_BROADBAND_BEARER)

typedef struct {
    MMBaseModem *modem;
    MMPortSerialAt *primary;
    MMPort *data;
    gchar *apn;
    guint step;
    gboolean activation_attempted;
    GError *error;
} ConnectContext;

static void
context_free (ConnectContext *ctx)
{
    g_object_set_data (G_OBJECT (ctx->modem), "w202ds-cid1-pending", NULL);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->primary);
    g_object_unref (ctx->data);
    g_free (ctx->apn);
    g_clear_error (&ctx->error);
    g_free (ctx);
}

static void connect_step (GTask *task);

static void
rollback_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    GError *error = NULL;

    mm_base_modem_at_command_full_finish (modem, res, &error);
    if (error) {
        mm_obj_warn (modem, "CID1 cleanup failed: %s", error->message);
        g_prefix_error (&ctx->error, "CID1 cleanup also failed; retry disconnect before reuse. ");
        g_error_free (error);
    }
    g_task_return_error (task, g_steal_pointer (&ctx->error));
    g_object_unref (task);
}

static void
connect_fail (GTask *task, GError *error)
{
    ConnectContext *ctx = g_task_get_task_data (task);

    if (ctx->activation_attempted) {
        ctx->error = error;
        /* Never pass the cancelled request to cleanup. */
        mm_base_modem_at_command_full (ctx->modem, MM_IFACE_PORT_AT (ctx->primary),
                                       "+CGACT=0,1", 45, FALSE, FALSE, NULL,
                                       (GAsyncReadyCallback) rollback_ready, task);
        return;
    }
    g_task_return_error (task, error);
    g_object_unref (task);
}

static gboolean
usable_ipv4 (const gchar *text)
{
    g_autoptr(GInetAddress) address = text ? g_inet_address_new_from_string (text) : NULL;
    const guint8 *bytes;

    if (!address || g_inet_address_get_family (address) != G_SOCKET_FAMILY_IPV4)
        return FALSE;
    bytes = g_inet_address_to_bytes (address);
    return bytes[0] != 0 && bytes[0] != 127 && bytes[0] < 224;
}

static void
command_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    GError *error = NULL;
    const gchar *response = mm_base_modem_at_command_full_finish (modem, res, &error);

    if (!error && g_task_get_cancellable (task))
        g_cancellable_set_error_if_cancelled (g_task_get_cancellable (task), &error);
    if (error) {
        connect_fail (task, error);
        return;
    }
    if (ctx->step == 0) {
        GList *contexts, *item;
        gboolean active = FALSE;

        contexts = mm_3gpp_parse_cgact_read_response (response, &error);
        if (error) {
            connect_fail (task, error);
            return;
        }
        for (item = contexts; item; item = item->next) {
            MM3gppPdpContextActive *pdp = item->data;
            if (pdp->cid == 1 && pdp->active)
                active = TRUE;
        }
        mm_3gpp_pdp_context_active_list_free (contexts);
        if (active) {
            connect_fail (task, g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
                                                    "CID1 already active; stop its current owner first"));
            return;
        }
    } else if (ctx->step == 4) {
        g_autofree gchar *address = NULL;
        g_autofree gchar *dns1 = NULL;
        g_autofree gchar *dns2 = NULL;
        g_autoptr(MMBearerIpConfig) config = NULL;
        MMBearerConnectResult *result;
        const gchar *dns[3] = { NULL, NULL, NULL };
        guint cid = 0, count = 0;

        if (!mm_3gpp_parse_cgcontrdp_response (response, &cid, NULL, NULL, &address,
                                               NULL, NULL, &dns1, &dns2, &error)) {
            connect_fail (task, error);
            return;
        }
        if (cid != 1 || !usable_ipv4 (address)) {
            connect_fail (task, g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                                    "Invalid CID1 IPv4 configuration"));
            return;
        }
        if (usable_ipv4 (dns1))
            dns[count++] = dns1;
        if (usable_ipv4 (dns2) && g_strcmp0 (dns1, dns2))
            dns[count++] = dns2;
        if (!count) {
            connect_fail (task, g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                                    "CID1 has no valid IPv4 DNS server"));
            return;
        }
        config = mm_bearer_ip_config_new ();
        mm_bearer_ip_config_set_method (config, MM_BEARER_IP_METHOD_STATIC);
        mm_bearer_ip_config_set_address (config, address);
        /* M-ETHER is an L3 link; do not treat the carrier subnet as local LAN. */
        mm_bearer_ip_config_set_prefix (config, 32);
        mm_bearer_ip_config_set_dns (config, dns);
        result = mm_bearer_connect_result_new (ctx->data, config, NULL);
        mm_bearer_connect_result_set_profile_id (result, 1);
        g_task_return_pointer (task, result, (GDestroyNotify) mm_bearer_connect_result_unref);
        g_object_unref (task);
        return;
    }
    ctx->step++;
    connect_step (task);
}

static void
connect_step (GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    g_autofree gchar *define = NULL;
    const gchar *command;

    switch (ctx->step) {
    case 0: command = "+CGACT?"; break;
    case 1:
        define = g_strdup_printf ("+CGDCONT=1,\"IP\",\"%s\",\"\",0,0,0,0,1", ctx->apn);
        command = define;
        break;
    case 2:
        ctx->activation_attempted = TRUE;
        command = "+CGACT=1,1";
        break;
    case 3: command = "+CGDATA=\"M-ETHER\",1"; break;
    case 4: command = "+CGCONTRDP=1"; break;
    default: g_assert_not_reached ();
    }
    /* Finish each modem command before checking cancellation, then roll back.
     * This avoids interpreting a late activation reply as the next command. */
    mm_base_modem_at_command_full (ctx->modem, MM_IFACE_PORT_AT (ctx->primary),
                                   command, ctx->step == 2 || ctx->step == 3 ? 45 : 5,
                                   FALSE, FALSE, NULL,
                                   (GAsyncReadyCallback) command_ready, task);
}

static void
connect_3gpp (MMBroadbandBearer *self, MMBroadbandModem *modem,
              MMPortSerialAt *primary, MMPortSerialAt *secondary,
              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);
    MMBearerProperties *properties = mm_base_bearer_peek_config (MM_BASE_BEARER (self));
    const gchar *apn = mm_bearer_properties_get_apn (properties);
    const gchar *user = mm_bearer_properties_get_user (properties);
    const gchar *password = mm_bearer_properties_get_password (properties);
    MMBearerIpFamily family = mm_bearer_properties_get_ip_type (properties);
    gint cid = mm_bearer_properties_get_profile_id (properties);
    MMPort *data;
    ConnectContext *ctx;

    /* Complete only after cleanup, even when cancellation arrived meanwhile. */
    g_task_set_check_cancellable (task, FALSE);
    if (!apn || !*apn || strlen (apn) > 100 ||
        (cid != MM_3GPP_PROFILE_ID_UNKNOWN && cid != 1) ||
        (family != MM_BEARER_IP_FAMILY_NONE && family != MM_BEARER_IP_FAMILY_IPV4) ||
        (user && *user) || (password && *password) ||
        strspn (apn, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != strlen (apn)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "W202DS supports CID1 IPv4 with an APN and no username/password");
        g_object_unref (task);
        return;
    }
    if (g_task_get_cancellable (task) && g_cancellable_is_cancelled (cancellable)) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Connection cancelled");
        g_object_unref (task);
        return;
    }
    if (g_object_get_data (G_OBJECT (modem), "w202ds-cid1-pending")) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
                                 "Another CID1 connection is in progress");
        g_object_unref (task);
        return;
    }
    data = mm_base_modem_peek_best_data_port (MM_BASE_MODEM (modem), MM_PORT_TYPE_NET);
    if (!data || g_strcmp0 (mm_port_get_device (data), "sipa_eth0")) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_CONNECTED,
                                 "No free sipa_eth0 data port");
        g_object_unref (task);
        return;
    }
    g_object_set_data (G_OBJECT (modem), "w202ds-cid1-pending", GINT_TO_POINTER (1));
    ctx = g_new0 (ConnectContext, 1);
    ctx->modem = g_object_ref (MM_BASE_MODEM (modem));
    ctx->primary = g_object_ref (primary);
    ctx->data = g_object_ref (data);
    ctx->apn = g_strdup (apn);
    g_task_set_task_data (task, ctx, (GDestroyNotify) context_free);
    connect_step (task);
}

static MMBearerConnectResult *
connect_finish (MMBroadbandBearer *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
disconnect_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    mm_base_modem_at_command_full_finish (modem, res, &error);
    if (error)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_3gpp (MMBroadbandBearer *self, MMBroadbandModem *modem,
                 MMPortSerialAt *primary, MMPortSerialAt *secondary, MMPort *data,
                 guint cid, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    if (cid != 1) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "Only CID1 belongs to this bearer");
        g_object_unref (task);
        return;
    }
    mm_base_modem_at_command_full (MM_BASE_MODEM (modem), MM_IFACE_PORT_AT (primary),
                                   "+CGACT=0,1", 45, FALSE, FALSE, NULL,
                                   (GAsyncReadyCallback) disconnect_ready, task);
}

static gboolean
disconnect_finish (MMBroadbandBearer *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void mm_broadband_bearer_w202ds_init (MMBroadbandBearerW202ds *self) {}
static void
mm_broadband_bearer_w202ds_class_init (MMBroadbandBearerW202dsClass *klass)
{
    MMBroadbandBearerClass *bearer = MM_BROADBAND_BEARER_CLASS (klass);
    bearer->connect_3gpp = connect_3gpp;
    bearer->connect_3gpp_finish = connect_finish;
    bearer->disconnect_3gpp = disconnect_3gpp;
    bearer->disconnect_3gpp_finish = disconnect_finish;
}
