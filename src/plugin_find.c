/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kdeconnect.findmyphone.request, an empty packet in either direction
 * (plugins/findmyphone and plugins/findthisdevice upstream, and
 * and/plugins/findmyphone, findremotedevice):
 *
 *   we send it:      the phone rings at full volume until dismissed
 *                    ("find-phone" request on the control socket);
 *   the phone sends: this computer plays a sound.
 *
 * Unlike a ping this is not a notification: the point is the noise. The
 * upstream plays a ring tone once and unmutes the sinks that were muted;
 * here the sound repeats (a generated beep pattern, or [find] sound) for
 * up to 30 s, until the notification is closed or its button pressed, or
 * "stop-ring" arrives on the control socket. The sound comes from an
 * external player (paplay, pw-play or aplay; [find] player overrides)
 * so the daemon needs no audio library. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "util.h"

#define FIND "kdeconnect.findmyphone.request"
#define MAX_RING_S 30.0
#define RATE 22050

static const char *const types[] = {FIND, NULL};

static struct {
    bool ringing;
    double end;       /* monotonic: stop then */
    char wav[KR_PATH_MAX];
    bool own_wav;     /* we made it and delete it */
    char player[32];
    char sound[KR_PATH_MAX]; /* what the player plays */
    kr_proc *proc;
    kr_note *note;
    bool unmuted;     /* we unmuted the default sink and give it back */
    bool source;
} ring;

/* ---- the generated sound ---------------------------------------------------- */

static void put16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
}

static void put32(FILE *f, unsigned v)
{
    put16(f, v & 0xffff);
    put16(f, v >> 16);
}

/* A mono 16-bit WAV of six cycles of "beep beep beep ... pause" (9 s). */
bool kr_find_write_wav(const char *path)
{
    const int cycles = 6;
    const double beep = 0.18, gap = 0.12, rest = 0.6;
    int per_cycle = (int)((3 * (beep + gap) + rest) * RATE);
    unsigned total = (unsigned)(per_cycle * cycles);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fputs("RIFF", f);
    put32(f, 36 + total * 2);
    fputs("WAVEfmt ", f);
    put32(f, 16);
    put16(f, 1); /* PCM */
    put16(f, 1); /* mono */
    put32(f, RATE);
    put32(f, RATE * 2);
    put16(f, 2);
    put16(f, 16);
    fputs("data", f);
    put32(f, total * 2);
    for (int c = 0; c < cycles; c++)
        for (int i = 0; i < per_cycle; i++) {
            double t = (double)i / RATE, v = 0;
            for (int b = 0; b < 3; b++) {
                double start = b * (beep + gap);
                if (t >= start && t < start + beep) {
                    double u = t - start, env = 1;
                    if (u < 0.005)
                        env = u / 0.005; /* no clicks */
                    else if (u > beep - 0.005)
                        env = (beep - u) / 0.005;
                    v = 0.6 * env * sin(2 * M_PI * 1000 * u);
                }
            }
            put16(f, (unsigned)(short)(v * 32767));
        }
    return fclose(f) == 0;
}

/* ---- the ring ------------------------------------------------------------------ */

static void ring_stop(kr_daemon *d);

static void play_once(kr_daemon *d);

static void on_player_exit(void *ud, int status, const char *out, size_t len)
{
    (void)status;
    (void)out;
    (void)len;
    kr_daemon *d = ud;
    ring.proc = NULL;
    if (ring.ringing && kr_now() < ring.end)
        play_once(d);
}

static void play_once(kr_daemon *d)
{
    const char *argv[4] = {ring.player, NULL, NULL, NULL};
    if (strcmp(ring.player, "aplay") == 0) {
        argv[1] = "-q";
        argv[2] = ring.sound;
    } else {
        argv[1] = ring.sound;
    }
    ring.proc = kr_proc_spawn(kr_daemon_procs(d), argv, false,
                              &(kr_proc_cb){.on_exit = on_player_exit}, d);
    if (!ring.proc)
        kr_log_warn(_("cannot play the sound with %s"), ring.player);
}

static double src_deadline(void *ud)
{
    (void)ud;
    return ring.ringing ? ring.end : 0;
}

static void src_dispatch(void *ud, const struct pollfd *fds, int n)
{
    (void)fds;
    (void)n;
    if (ring.ringing && kr_now() >= ring.end)
        ring_stop(ud);
}

static void on_check_mute(void *ud, int status, const char *out, size_t len)
{
    (void)len;
    kr_daemon *d = ud;
    /* LC_ALL=C: "Mute: yes". */
    if (status == 0 && out && strstr(out, "yes") && ring.ringing) {
        const char *argv[] = {"pactl", "set-sink-mute", "@DEFAULT_SINK@", "0", NULL};
        if (kr_proc_spawn(kr_daemon_procs(d), argv, false, NULL, NULL))
            ring.unmuted = true;
    }
}

static void note_action(void *ud, const char *key)
{
    (void)key;
    ring_stop(ud);
}

static void note_closed(void *ud, int reason)
{
    (void)reason;
    kr_daemon *d = ud;
    ring.note = NULL;
    ring_stop(d);
}

static void ring_stop(kr_daemon *d)
{
    if (!ring.ringing)
        return;
    ring.ringing = false;
    if (ring.proc) {
        kr_proc_detach(ring.proc);
        kr_proc_kill(ring.proc);
        ring.proc = NULL;
    }
    if (ring.note) {
        kr_note_close(ring.note);
        kr_note_detach(ring.note);
        ring.note = NULL;
    }
    if (ring.unmuted) {
        const char *argv[] = {"pactl", "set-sink-mute", "@DEFAULT_SINK@", "1", NULL};
        kr_proc_spawn(kr_daemon_procs(d), argv, false, NULL, NULL);
        ring.unmuted = false;
    }
    if (ring.own_wav)
        unlink(ring.wav);
    ring.own_wav = false;
}

static void ring_start(kr_daemon *d, const char *who)
{
    if (ring.ringing) {
        ring.end = kr_now() + MAX_RING_S;
        return;
    }
    const kr_config *cfg = kr_daemon_config(d);
    const char *sound = kr_config_get(cfg, "find", "sound");
    const char *player = kr_config_get(cfg, "find", "player");
    if (!player || !*player) {
        static const char *const known[] = {"paplay", "pw-play", "aplay"};
        for (size_t i = 0; i < 3 && (!player || !*player); i++)
            if (kr_which(known[i]))
                player = known[i];
    }
    ring.own_wav = false;
    if (sound && *sound) {
        kr_pathf(ring.sound, sizeof ring.sound, "%s", sound);
    } else if (kr_runtime_path(ring.wav, sizeof ring.wav, "kiremote-find.wav") &&
               kr_find_write_wav(ring.wav)) {
        ring.own_wav = true;
        kr_pathf(ring.sound, sizeof ring.sound, "%s", ring.wav);
    } else {
        ring.sound[0] = '\0';
    }

    ring.ringing = true;
    ring.end = kr_now() + MAX_RING_S;
    if (!ring.source) {
        ring.source = kr_daemon_add_source(d, &(kr_source){NULL, src_dispatch, src_deadline}, d);
    }

    if (player && *player && ring.sound[0]) {
        kr_pathf(ring.player, sizeof ring.player, "%s", player);
        const char *argv[] = {"pactl", "get-sink-mute", "@DEFAULT_SINK@", NULL};
        kr_proc_spawn(kr_daemon_procs(d), argv, true, &(kr_proc_cb){.on_exit = on_check_mute}, d);
        play_once(d);
    } else {
        kr_log_warn(_("no way to play a sound (install paplay, pw-play or aplay)"));
    }

    char title[200];
    snprintf(title, sizeof title, _("%s is looking for this computer"), who);
    static const char *const actions[] = {"stop", N_("Stop"), NULL};
    const char *translated[3] = {actions[0], _(actions[1]), NULL};
    ring.note = kr_notify_show(kr_daemon_notify(d),
                               &(kr_note_spec){.summary = title,
                                               .body = kr_notify_has_actions(kr_daemon_notify(d))
                                                           ? NULL
                                                           : _("Close this notification to stop the sound."),
                                               .icon = "audio-volume-high",
                                               .actions = translated,
                                               .timeout_ms = 0,
                                               .urgent = true},
                               &(kr_note_cb){.on_action = note_action, .on_closed = note_closed}, d);
}

/* ---- the plugin ------------------------------------------------------------------ */

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)p;
    ring_start(d, kr_device_name(dev));
}

static void shutdown_plugin(kr_daemon *d)
{
    ring_stop(d);
    if (ring.source)
        kr_daemon_remove_source(d, d);
    ring.source = false;
}

static bool request(kr_daemon *d, const char *type, const cJSON *req, cJSON *reply)
{
    if (strcmp(type, "stop-ring") == 0) {
        ring_stop(d);
        return true;
    }
    if (strcmp(type, "find-phone") != 0)
        return false;
    kr_device *dev = kr_daemon_request_device(d, req, reply);
    if (!dev)
        return true;
    kr_packet *p = kr_packet_new(FIND);
    bool ok = p && kr_daemon_send(d, dev, p);
    kr_packet_free(p);
    if (!ok)
        kr_control_fail(reply, kr_device_paired(dev) ? _("the device is not reachable or does not take this")
                                                     : _("the device is not paired"));
    return true;
}

const kr_plugin kr_plugin_find = {
    .name = "findmyphone",
    .incoming = types,
    .outgoing = types,
    .shutdown = shutdown_plugin,
    .on_packet = on_packet,
    .request = request,
};
