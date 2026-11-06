/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2008 - 2010 Dan Williams <dcbw@redhat.com>
 */

#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>

#include <gmodule.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-plugin-common.h"
#include "mm-broadband-w202ds.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-serial-parsers.h"
#include "mm-log-object.h"

#define MM_TYPE_PLUGIN_W202DS mm_plugin_w202ds_get_type ()
MM_DEFINE_PLUGIN (W202DS, w202ds, W202ds)

/*****************************************************************************/

static MMBaseModem *
create_modem (MMPlugin *self,
              const gchar *uid,
              const gchar *physdev,
              const gchar **drivers,
              guint16 vendor,
              guint16 product,
              guint16 subsystem_vendor,
              guint16 subsystem_device,
              GList *probes,
              GError **error)
{
    return g_object_new (mm_broadband_modem_w202ds_get_type (),
                         MM_BASE_MODEM_DEVICE, uid,
                         MM_BASE_MODEM_PHYSDEV, physdev,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, mm_plugin_get_name (self),
                         MM_BASE_MODEM_VENDOR_ID, vendor,
                         MM_BASE_MODEM_PRODUCT_ID, product,
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_3GPP_CS_NETWORK_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_3GPP_PS_NETWORK_SUPPORTED, FALSE,
                         NULL);
}

/*****************************************************************************/

MM_PLUGIN_NAMED_CREATOR_SCOPE MMPlugin *
mm_plugin_create_w202ds (void)
{
    static const gchar *subsystems[] = { "spipe", "net", NULL };

    static const gchar *tags[] = { "ID_MM_W202DS", NULL };

    return MM_PLUGIN (
        g_object_new (MM_TYPE_PLUGIN_W202DS,
                      MM_PLUGIN_NAME,               MM_MODULE_NAME,
                      MM_PLUGIN_ALLOWED_UDEV_TAGS,  tags,
                      MM_PLUGIN_ALLOWED_SUBSYSTEMS, subsystems,
                      MM_PLUGIN_ALLOW_UNBOUND_PORTS, TRUE,
                      MM_PLUGIN_ALLOWED_AT,         TRUE,
                      NULL));
}

static void
mm_plugin_w202ds_init (MMPluginW202ds *self)
{
}

static void
mm_plugin_w202ds_class_init (MMPluginW202dsClass *klass)
{
    MMPluginClass *plugin_class = MM_PLUGIN_CLASS (klass);

    plugin_class->create_modem = create_modem;
}
