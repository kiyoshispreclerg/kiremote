/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pause the desktop's players while a call rings (plugins/pausemusic
 * upstream). It listens to the same kdeconnect.telephony packets as the
 * telephony plugin: "ringing" or "talking" pauses every player that is
 * playing; the packet with isCancel resumes the ones we paused.
 *
 *   [pausemusic]
 *   only_when_talking = false   # wait for the call to be answered
 *   resume = true               # play again when the call ends
 *
 * The upstream also has an option to mute the system volume instead; not
 * here yet. */
#include <stdio.h>
#include <string.h>

#include "plugin.h"

#define TELEPHONY "kdeconnect.telephony"
#define MAX_PAUSED 16

static const char *const incoming[] = {TELEPHONY, NULL};

static struct {
    char names[MAX_PAUSED][160];
    int n;
} paused;

static void on_connect(kr_daemon *d, kr_device *dev)
{
    (void)dev;
    kr_daemon_mpris(d); /* start tracking, so the statuses are known when a call comes */
}

static bool was_paused(const char *name)
{
    for (int i = 0; i < paused.n; i++)
        if (strcmp(paused.names[i], name) == 0)
            return true;
    return false;
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)dev;
    kr_mpris *m = kr_daemon_mpris(d);
    const char *event = kr_packet_str(p, "event");
    if (!m || !event)
        return;
    const kr_config *cfg = kr_daemon_config(d);
    bool only_talking = kr_config_get_bool(cfg, "pausemusic", "only_when_talking", false);
    bool talking = strcmp(event, "talking") == 0, ringing = strcmp(event, "ringing") == 0;
    if (only_talking ? !talking : !(talking || ringing))
        return;

    if (!kr_packet_bool(p, "isCancel", false)) {
        for (int i = 0; i < kr_mpris_count(m); i++) {
            const kr_player *pl = kr_mpris_at(m, i);
            if (strcmp(pl->status, "Playing") != 0 || was_paused(pl->name) || paused.n == MAX_PAUSED)
                continue;
            snprintf(paused.names[paused.n++], sizeof paused.names[0], "%s", pl->name);
            kr_mpris_action(m, pl, pl->can_pause ? "Pause" : "Stop");
        }
        return;
    }
    if (kr_config_get_bool(cfg, "pausemusic", "resume", true))
        for (int i = 0; i < paused.n; i++) {
            const kr_player *pl = kr_mpris_find(m, paused.names[i]);
            if (pl)
                kr_mpris_action(m, pl, "Play");
        }
    paused.n = 0;
}

const kr_plugin kr_plugin_pausemusic = {
    .name = "pausemusic",
    .incoming = incoming,
    .on_connect = on_connect,
    .on_packet = on_packet,
};
