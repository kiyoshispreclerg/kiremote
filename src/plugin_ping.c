/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kdeconnect.ping, both ways: {"message": "..."} is optional
 * (plugins/ping/pingplugin.cpp upstream). Phase 1 only logs what arrives;
 * the notification comes with D-Bus in phase 2. */
#include "i18n.h"
#include "log.h"
#include "plugin.h"

#define PING "kdeconnect.ping"

static const char *const types[] = {PING, NULL};

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)d;
    const char *msg = kr_packet_str(p, "message");
    kr_log_info(_("ping from %s: %s"), kr_device_name(dev), msg ? msg : _("Ping!"));
}

bool kr_ping_send(kr_daemon *d, kr_device *dev, const char *message)
{
    kr_packet *p = kr_packet_new(PING);
    if (!p)
        return false;
    if (message && *message)
        cJSON_AddStringToObject(p->body, "message", message);
    bool ok = kr_daemon_send(d, dev, p);
    kr_packet_free(p);
    return ok;
}

const kr_plugin kr_plugin_ping = {
    .name = "ping",
    .incoming = types,
    .outgoing = types,
    .on_packet = on_packet,
};
