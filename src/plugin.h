/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_PLUGIN_H
#define KR_PLUGIN_H

/* A plugin: the packet types it takes and sends, and what to do with
 * them. One file per plugin (plugin_*.c); kr_plugins lists them, and our
 * identity's capabilities come from that list. Packets only reach plugins
 * from paired devices. */

#include "cJSON.h"
#include "control.h"
#include "daemon.h"
#include "packet.h"

typedef struct kr_plugin {
    const char *name; /* also the key in the [plugins] section */
    const char *const *incoming; /* NULL-terminated */
    const char *const *outgoing;
    /* Bytes of state kept for each device while it is connected and paired
     * (zeroed on connect, dropped after on_disconnect). */
    size_t state_size;
    /* Once, when the daemon starts (plugin enabled); optional. */
    void (*init)(kr_daemon *d);
    void (*shutdown)(kr_daemon *d);
    /* A paired device became reachable, and went away. */
    void (*on_connect)(kr_daemon *d, kr_device *dev);
    void (*on_disconnect)(kr_daemon *d, kr_device *dev);
    void (*on_packet)(kr_daemon *d, kr_device *dev, const kr_packet *p);
    /* A control socket request this plugin knows (see control.h): true if
     * it handled it, whether it worked or not (then reply says). */
    bool (*request)(kr_daemon *d, const char *type, const cJSON *req, cJSON *reply);
    /* Adds what it knows about the device to its entry in the "devices"
     * reply (battery level, signal...). */
    void (*describe)(kr_daemon *d, const kr_device *dev, cJSON *out);
} kr_plugin;

/* The state of plugin pl for dev (state_size bytes), NULL if dev is not up. */
void *kr_device_state(kr_device *dev, const kr_plugin *pl);

/* NULL-terminated. */
extern const kr_plugin *const kr_plugins[];

/* Our incomingCapabilities and outgoingCapabilities, from the plugins that
 * are on in cfg. */
cJSON *kr_plugins_capabilities(const kr_config *cfg, bool incoming);
/* Does pl take packets of this type? */
bool kr_plugin_takes(const kr_plugin *pl, const char *type);

extern const kr_plugin kr_plugin_battery;
extern const kr_plugin kr_plugin_connectivity;
extern const kr_plugin kr_plugin_find;
extern const kr_plugin kr_plugin_telephony;
extern const kr_plugin kr_plugin_mpris;
extern const kr_plugin kr_plugin_volume;

/* systemvolume: the outputs, from `pactl --format=json list sinks`. */
#define KR_MAX_SINKS 16
typedef struct {
    char name[256];
    char description[256];
    int volume; /* 0..65536 is 0..100% */
    bool muted, enabled; /* enabled: it is the default output */
} kr_sink;
/* Parses the JSON into out (at most max); returns how many. default_name
 * (may be NULL) marks the enabled one. */
int kr_sinks_parse(const char *json, const char *default_name, kr_sink *out, int max);
extern const kr_plugin kr_plugin_pausemusic;

/* find: writes the generated ring sound (a WAV) to path. */
bool kr_find_write_wav(const char *path);

/* battery: the system battery, read from a power_supply directory (tests
 * point it at a fake one). False when it has no real battery. */
typedef struct {
    int count;   /* batteries that are part of this computer */
    int charge;  /* percent, averaged over them */
    bool charging, discharging;
} kr_battery_info;
bool kr_battery_read(const char *dir, kr_battery_info *out);

/* ping */
bool kr_ping_send(kr_daemon *d, kr_device *dev, const char *message);

/* share: sends each file in paths (absolute); failures go into errors
 * (one string each). Returns how many were sent. */
int kr_share_files(kr_daemon *d, kr_device *dev, const cJSON *paths, cJSON *errors);

#endif
