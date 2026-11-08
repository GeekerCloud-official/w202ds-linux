/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mm-broadband-w202ds.h"
#include "mm-iface-modem.h"
#include "mm-bearer-list.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-log-object.h"
#include "mm-modem-helpers-w202ds.h"
#include <string.h>

typedef struct {
    MMBroadbandModem parent;
    GRegex *sim_regex;
    guint sim_check_source;
    guint sim_generation;
    gboolean sim_hot_swap_active;
    gboolean sim_check_running;
    gboolean sim_check_pending;
    gboolean sim_check_identity;
    gulong sim_registration_signal;
    gulong sim_state_signal;
} MMBroadbandModemW202ds;
typedef struct { MMBroadbandModemClass parent; } MMBroadbandModemW202dsClass;

static MMIfaceModemInterface *iface_modem_parent;
static void iface_modem_init (MMIfaceModemInterface *iface);
G_DEFINE_TYPE_EXTENDED (MMBroadbandModemW202ds, mm_broadband_modem_w202ds,
                       MM_TYPE_BROADBAND_MODEM, 0,
                       G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init))

/* Keep the AT port open even in SIM_MISSING, so insertion can be detected
 * without restarting ModemManager or adding a polling service. */
typedef struct {
    guint generation;
    gboolean check_identity;
} SimCheckContext;

static void schedule_sim_check (MMBroadbandModemW202ds *self);

static void
sim_check_complete (GTask *task)
{
    MMBroadbandModemW202ds *self = g_task_get_source_object (task);
    SimCheckContext *ctx = g_task_get_task_data (task);

    if (ctx->generation == self->sim_generation) {
        self->sim_check_running = FALSE;
        schedule_sim_check (self);
    }
    g_object_unref (task);
}

static void
sim_identity_ready (MMIfaceModem *modem, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError) error = NULL;

    if (!mm_iface_modem_check_for_sim_swap_finish (modem, res, &error))
        mm_obj_dbg (modem, "SIM identity check failed: %s", error->message);
    sim_check_complete (task);
}

static void
sim_presence_ready (MMIfaceModem *modem, GAsyncResult *res, GTask *task)
{
    MMBroadbandModemW202ds *self = g_task_get_source_object (task);
    SimCheckContext *ctx = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    g_autoptr(MMBaseSim) sim = NULL;
    g_autoptr(MmGdbusModemSkeleton) skeleton = NULL;
    MMModemState state = MM_MODEM_STATE_UNKNOWN;
    MMModemStateFailedReason failed_reason = MM_MODEM_STATE_FAILED_REASON_NONE;
    MMModemLock lock;

    lock = iface_modem_parent->load_unlock_required_finish (modem, res, &error);
    if (!self->sim_hot_swap_active || ctx->generation != self->sim_generation) {
        sim_check_complete (task);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_SIM, &sim,
                  MM_IFACE_MODEM_STATE, &state,
                  MM_IFACE_MODEM_DBUS_SKELETON, &skeleton,
                  NULL);
    if (skeleton)
        failed_reason = mm_gdbus_modem_get_state_failed_reason (MM_GDBUS_MODEM (skeleton));

    switch (mm_w202ds_sim_check_action (sim != NULL, state, failed_reason, lock, error)) {
    case MM_W202DS_SIM_ACTION_REMOVED:
    case MM_W202DS_SIM_ACTION_INSERTED:
        /* The standard flow disables bearers, invalidates old SIM/registration
         * objects and reprobes. NM then removes the stale data connection. */
        mm_iface_modem_process_sim_event (modem);
        break;
    case MM_W202DS_SIM_ACTION_CHECK_IDENTITY:
        if (ctx->check_identity) {
            mm_iface_modem_check_for_sim_swap (modem,
                                              (GAsyncReadyCallback) sim_identity_ready,
                                              task);
            return;
        }
        break;
    case MM_W202DS_SIM_ACTION_NONE:
        if (error)
            mm_obj_dbg (self, "SIM presence check inconclusive: %s", error->message);
        break;
    default:
        g_assert_not_reached ();
    }
    sim_check_complete (task);
}

static gboolean
sim_check_idle (MMBroadbandModemW202ds *self)
{
    GTask *task;
    SimCheckContext *ctx;

    self->sim_check_source = 0;
    self->sim_check_running = TRUE;
    self->sim_check_pending = FALSE;
    task = g_task_new (self, NULL, NULL, NULL);
    ctx = g_new0 (SimCheckContext, 1);
    ctx->generation = self->sim_generation;
    ctx->check_identity = self->sim_check_identity;
    self->sim_check_identity = FALSE;
    g_task_set_task_data (task, ctx, g_free);

    /* The inherited implementation sends an uncached CPIN query through MM's
     * serialized command queue; never open the SIPC channel independently. */
    iface_modem_parent->load_unlock_required (MM_IFACE_MODEM (self), FALSE, NULL,
                                             (GAsyncReadyCallback) sim_presence_ready,
                                             task);
    return G_SOURCE_REMOVE;
}

static void
schedule_sim_check (MMBroadbandModemW202ds *self)
{
    MMModemState state = MM_MODEM_STATE_UNKNOWN;

    if (!self->sim_hot_swap_active || !self->sim_check_pending ||
        self->sim_check_running || self->sim_check_source)
        return;
    g_object_get (self, MM_IFACE_MODEM_STATE, &state, NULL);
    /* Initialization performs its own CPIN query. Preserve hints until that
     * completes, instead of reprobeing an object that is still initializing. */
    if (state == MM_MODEM_STATE_UNKNOWN || state == MM_MODEM_STATE_INITIALIZING)
        return;
    self->sim_check_source = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                             (GSourceFunc) sim_check_idle,
                                             g_object_ref (self), g_object_unref);
}

static void
sim_unsolicited (MMPortSerialAt *port, GMatchInfo *match_info, MMBroadbandModemW202ds *self)
{
    self->sim_check_pending = TRUE;
    self->sim_check_identity = TRUE;
    /* Defer work until the serial parser has finished iterating its handlers. */
    schedule_sim_check (self);
}

static void
sim_registration_changed (MMBroadbandModemW202ds *self, GParamSpec *pspec, gpointer unused)
{
    /* Registration loss is only a hint, never proof of removal. This also
     * catches a missed ECIND event without periodic background queries. */
    self->sim_check_pending = TRUE;
    schedule_sim_check (self);
}

static void
sim_state_changed (MMBroadbandModemW202ds *self, GParamSpec *pspec, gpointer unused)
{
    schedule_sim_check (self);
}

static void
cleanup_sim_hot_swap (MMIfaceModem *modem)
{
    MMBroadbandModemW202ds *self = (MMBroadbandModemW202ds *) modem;
    MMPortSerialAt *ports[2];
    guint i;

    self->sim_hot_swap_active = FALSE;
    self->sim_generation++;
    self->sim_check_running = FALSE;
    self->sim_check_pending = FALSE;
    self->sim_check_identity = FALSE;
    if (self->sim_check_source) {
        g_source_remove (self->sim_check_source);
        self->sim_check_source = 0;
    }
    if (self->sim_registration_signal) {
        g_signal_handler_disconnect (self, self->sim_registration_signal);
        self->sim_registration_signal = 0;
    }
    if (self->sim_state_signal) {
        g_signal_handler_disconnect (self, self->sim_state_signal);
        self->sim_state_signal = 0;
    }
    ports[0] = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));
    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (ports[i])
            mm_port_serial_at_add_unsolicited_msg_handler (ports[i], self->sim_regex,
                                                          NULL, NULL, NULL);
    }
    mm_broadband_modem_sim_hot_swap_ports_context_reset (MM_BROADBAND_MODEM (self));
}

static void
setup_sim_hot_swap (MMIfaceModem *modem, GAsyncReadyCallback callback, gpointer user_data)
{
    MMBroadbandModemW202ds *self = (MMBroadbandModemW202ds *) modem;
    g_autoptr(GTask) task = g_task_new (self, NULL, callback, user_data);
    g_autoptr(GError) error = NULL;
    MMPortSerialAt *ports[2];
    guint i;

    if (!mm_broadband_modem_sim_hot_swap_ports_context_init (MM_BROADBAND_MODEM (self), &error)) {
        g_task_return_error (task, g_steal_pointer (&error));
        return;
    }
    self->sim_generation++;
    self->sim_hot_swap_active = TRUE;
    ports[0] = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));
    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (ports[i])
            mm_port_serial_at_add_unsolicited_msg_handler (ports[i], self->sim_regex,
                                                          (MMPortSerialAtUnsolicitedMsgFn) sim_unsolicited,
                                                          self, NULL);
    }
    self->sim_registration_signal = g_signal_connect (self,
        "notify::" MM_IFACE_MODEM_3GPP_REGISTRATION_STATE,
        G_CALLBACK (sim_registration_changed), NULL);
    self->sim_state_signal = g_signal_connect (self, "notify::" MM_IFACE_MODEM_STATE,
                                              G_CALLBACK (sim_state_changed), NULL);
    g_object_set (self, MM_IFACE_MODEM_SIM_HOT_SWAP_SUPPORTED, TRUE, NULL);
    g_task_return_boolean (task, TRUE);
}

static gboolean
setup_sim_hot_swap_finish (MMIfaceModem *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

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
    iface_modem_parent = g_type_interface_peek_parent (iface);
    iface->setup_sim_hot_swap = setup_sim_hot_swap;
    iface->setup_sim_hot_swap_finish = setup_sim_hot_swap_finish;
    iface->cleanup_sim_hot_swap = cleanup_sim_hot_swap;
    iface->load_current_capabilities = load_current_capabilities;
    iface->load_current_capabilities_finish = load_current_capabilities_finish;
    iface->create_bearer = create_bearer;
    iface->create_bearer_finish = create_bearer_finish;
    iface->create_bearer_list = create_bearer_list;
    /* SIPC is packet transport: no UART flow control or DTR reset. */
    iface->setup_flow_control = NULL;
    iface->setup_flow_control_finish = NULL;
}

static void
dispose (GObject *object)
{
    cleanup_sim_hot_swap (MM_IFACE_MODEM (object));
    G_OBJECT_CLASS (mm_broadband_modem_w202ds_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMBroadbandModemW202ds *self = (MMBroadbandModemW202ds *) object;
    g_regex_unref (self->sim_regex);
    G_OBJECT_CLASS (mm_broadband_modem_w202ds_parent_class)->finalize (object);
}

static void
mm_broadband_modem_w202ds_init (MMBroadbandModemW202ds *self)
{
    self->sim_regex = mm_w202ds_sim_urc_regex_new ();
}

static void
mm_broadband_modem_w202ds_class_init (MMBroadbandModemW202dsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = dispose;
    object_class->finalize = finalize;
}
