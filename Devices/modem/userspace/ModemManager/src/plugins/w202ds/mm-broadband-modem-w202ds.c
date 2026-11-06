/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mm-broadband-w202ds.h"
#include "mm-iface-modem.h"
#include "mm-bearer-list.h"
#include "mm-base-modem-at.h"
#include <string.h>

typedef struct { MMBroadbandModem parent; } MMBroadbandModemW202ds;
typedef struct { MMBroadbandModemClass parent; } MMBroadbandModemW202dsClass;

static void iface_modem_init (MMIfaceModemInterface *iface);
G_DEFINE_TYPE_EXTENDED (MMBroadbandModemW202ds, mm_broadband_modem_w202ds,
                       MM_TYPE_BROADBAND_MODEM, 0,
                       G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init))

static void
bearer_ready (GObject *source, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    GObject *bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, &error);

    if (!bearer)
        g_task_return_error (task, error);
    else {
        mm_base_bearer_export (MM_BASE_BEARER (bearer));
        g_task_return_pointer (task, bearer, g_object_unref);
    }
    g_object_unref (task);
}

static void
create_bearer (MMIfaceModem *self, MMBearerProperties *properties,
               GAsyncReadyCallback callback, gpointer user_data)
{
    g_async_initable_new_async (mm_broadband_bearer_w202ds_get_type (),
                               G_PRIORITY_DEFAULT, NULL,
                               (GAsyncReadyCallback) bearer_ready,
                               g_task_new (self, NULL, callback, user_data),
                               MM_BASE_BEARER_MODEM, self,
                               MM_BASE_BEARER_CONFIG, properties, NULL);
}

static MMBaseBearer *
create_bearer_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static MMBearerList *
create_bearer_list (MMIfaceModem *self)
{
    return mm_bearer_list_new (1, 0);
}

/* Firmware reports only +CGSM in GCAP even when registered on NR.
 * Derive packet capabilities from the registration interfaces it implements. */
static void
nr_capability_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    const gchar *response = mm_base_modem_at_command_finish (modem, res, NULL);
    guint caps = GPOINTER_TO_UINT (g_task_get_task_data (task));
    if (response && strstr (response, "+C5GREG:"))
        caps |= MM_MODEM_CAPABILITY_5GNR;
    if (!caps)
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "No LTE/NR registration interface found");
    else
        g_task_return_int (task, caps);
    g_object_unref (task);
}

static void
lte_capability_ready (MMBaseModem *modem, GAsyncResult *res, GTask *task)
{
    const gchar *response = mm_base_modem_at_command_finish (modem, res, NULL);
    guint caps = response && strstr (response, "+CEREG:") ? MM_MODEM_CAPABILITY_LTE : 0;
    g_task_set_task_data (task, GUINT_TO_POINTER (caps), NULL);
    mm_base_modem_at_command (modem, "+C5GREG=?", 3, TRUE,
                              (GAsyncReadyCallback) nr_capability_ready, task);
}

static void
load_current_capabilities (MMIfaceModem *self, GAsyncReadyCallback callback, gpointer user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self), "+CEREG=?", 3, TRUE,
                              (GAsyncReadyCallback) lte_capability_ready,
                              g_task_new (self, NULL, callback, user_data));
}

static MMModemCapability
load_current_capabilities_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    gssize value = g_task_propagate_int (G_TASK (res), error);
    return value < 0 ? MM_MODEM_CAPABILITY_NONE : (MMModemCapability) value;
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface->load_current_capabilities = load_current_capabilities;
    iface->load_current_capabilities_finish = load_current_capabilities_finish;
    iface->create_bearer = create_bearer;
    iface->create_bearer_finish = create_bearer_finish;
    iface->create_bearer_list = create_bearer_list;
    /* SIPC is packet transport: no UART flow control or DTR reset. */
    iface->setup_flow_control = NULL;
    iface->setup_flow_control_finish = NULL;
}

static void mm_broadband_modem_w202ds_init (MMBroadbandModemW202ds *self) {}
static void mm_broadband_modem_w202ds_class_init (MMBroadbandModemW202dsClass *klass) {}
