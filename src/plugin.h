/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_PLUGIN_H
#define KR_PLUGIN_H

/* A plugin: the packet types it takes and sends, and what to do with
 * them. One file per plugin (plugin_*.c); kr_plugins lists them, and our
 * identity's capabilities come from that list. Packets only reach plugins
 * from paired devices. */

#include "cJSON.h"
#include "daemon.h"
#include "packet.h"

typedef struct {
    const char *name;
    const char *const *incoming; /* NULL-terminated */
    const char *const *outgoing;
    void (*on_packet)(kr_daemon *d, kr_device *dev, const kr_packet *p);
} kr_plugin;

/* NULL-terminated. */
extern const kr_plugin *const kr_plugins[];

/* Our incomingCapabilities and outgoingCapabilities, from kr_plugins. */
cJSON *kr_plugins_capabilities(bool incoming);
/* The plugin that takes type, or NULL. */
const kr_plugin *kr_plugin_for(const char *type);

/* ping */
bool kr_ping_send(kr_daemon *d, kr_device *dev, const char *message);

/* share: sends each file in paths (absolute); failures go into errors
 * (one string each). Returns how many were sent. */
int kr_share_files(kr_daemon *d, kr_device *dev, const cJSON *paths, cJSON *errors);

#endif
