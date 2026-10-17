/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kdeconnect.connectivity_report, received only (plugins/connectivity-report
 * upstream; and/plugins/connectivityreport for the sender):
 *
 *   {"signalStrengths": {"<subscription id>": {"networkType": "LTE",
 *                                                "signalStrength": 0-4}, ...}}
 *
 * The upstream shows the first subscription (a dual-SIM phone sends one
 * entry per SIM; the ids are opaque). Here the device's entry in the
 * "devices" reply has "signal": {"type", "strength"}. */
#include <stdio.h>
#include <string.h>

#include "plugin.h"

#define REPORT "kdeconnect.connectivity_report"

static const char *const incoming[] = {REPORT, NULL};

typedef struct {
    bool known;
    char type[16];
    int strength; /* 0-4; -1 no signal */
} signal_state;

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)d;
    signal_state *st = kr_device_state(dev, &kr_plugin_connectivity);
    const cJSON *all = cJSON_GetObjectItemCaseSensitive(p->body, "signalStrengths");
    const cJSON *first = cJSON_IsObject(all) ? all->child : NULL;
    if (!cJSON_IsObject(first))
        return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(first, "networkType");
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(first, "signalStrength");
    snprintf(st->type, sizeof st->type, "%s", cJSON_IsString(type) ? type->valuestring : "");
    st->strength = cJSON_IsNumber(level) ? (int)level->valuedouble : -1;
    st->known = true;
}

static void describe(kr_daemon *d, const kr_device *dev, cJSON *out)
{
    (void)d;
    signal_state *st = kr_device_state((kr_device *)dev, &kr_plugin_connectivity);
    if (!st || !st->known)
        return;
    cJSON *o = cJSON_AddObjectToObject(out, "signal");
    if (o) {
        cJSON_AddStringToObject(o, "type", st->type);
        cJSON_AddNumberToObject(o, "strength", st->strength);
    }
}

const kr_plugin kr_plugin_connectivity = {
    .name = "connectivity_report",
    .incoming = incoming,
    .state_size = sizeof(signal_state),
    .on_packet = on_packet,
    .describe = describe,
};
