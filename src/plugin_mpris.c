/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The phone controls the desktop's media players: kdeconnect.mpris out,
 * kdeconnect.mpris.request in (plugins/mpriscontrol upstream; the
 * Android client is and/plugins/mprisreceiver).
 *
 * We send:
 *   {"playerList": [names], "supportAlbumArtPayload": true}
 *   {"player", "title", "artist", "album", "albumArtUrl", "length" (ms),
 *    "url", "pos" (ms), "isPlaying", "canPause", "canPlay", "canGoNext",
 *    "canGoPrevious", "canSeek", "loopStatus", "shuffle", "volume" (0-100)}
 *     the changed groups on a change, everything on requestNowPlaying;
 *   {"player", "pos"} after a seek;
 *   {"transferringAlbumArt": true, "player", "albumArtUrl"} + payload.
 *
 * The phone asks with {"player", ...}: "action" (Play, Pause, PlayPause,
 * Stop, Next, Previous), "setVolume" (0-100), "setLoopStatus", "setShuffle",
 * "Seek" (µs, relative), "SetPosition" (ms), "requestNowPlaying",
 * "requestVolume", "requestPlayerList", "albumArtUrl".
 *
 * Differences from the upstream, in docs/UPSTREAM.md: only the listed
 * actions are executed; the plasma-browser-integration filter is by
 * service name. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "util.h"

#define MPRIS "kdeconnect.mpris"
#define MPRIS_REQUEST "kdeconnect.mpris.request"

static const char *const incoming[] = {MPRIS_REQUEST, NULL};
static const char *const outgoing[] = {MPRIS, NULL};

/* ---- the player list ---------------------------------------------------------- */

/* getFilteredPlayerKeys: when the browser has the Plasma integration
 * (which exposes each tab's media properly), its plain MPRIS player is a
 * duplicate and is left out. */
static bool hidden(const kr_mpris *m, const kr_player *p)
{
    bool ff = false, chrome = false;
    for (int i = 0; i < kr_mpris_count(m); i++) {
        const kr_player *q = kr_mpris_at(m, i);
        if (strncmp(q->service, "org.mpris.MediaPlayer2.plasma-browser-integration", 49) != 0)
            continue;
        ff |= strstr(q->name, "Firefox") != NULL;
        chrome |= strstr(q->name, "Chrome") != NULL || strstr(q->name, "Chromium") != NULL;
    }
    return (ff && strncmp(p->service, "org.mpris.MediaPlayer2.firefox", 30) == 0) ||
           (chrome && strncmp(p->service, "org.mpris.MediaPlayer2.chromium", 31) == 0);
}

static kr_packet *list_packet(const kr_mpris *m)
{
    kr_packet *p = kr_packet_new(MPRIS);
    cJSON *a = p ? cJSON_AddArrayToObject(p->body, "playerList") : NULL;
    if (!a) {
        kr_packet_free(p);
        return NULL;
    }
    for (int i = 0; i < kr_mpris_count(m); i++) {
        const kr_player *q = kr_mpris_at(m, i);
        if (!hidden(m, q))
            cJSON_AddItemToArray(a, cJSON_CreateString(q->name));
    }
    cJSON_AddBoolToObject(p->body, "supportAlbumArtPayload", true);
    return p;
}

static void send_list(kr_daemon *d, kr_device *only)
{
    kr_mpris *m = kr_daemon_mpris(d);
    kr_packet *p = m ? list_packet(m) : NULL;
    if (!p)
        return;
    if (only)
        kr_daemon_send(d, only, p);
    else
        for (int i = 0; i < kr_daemon_device_count(d); i++)
            kr_daemon_send(d, kr_daemon_device_at(d, i), p);
    kr_packet_free(p);
}

/* ---- state packets --------------------------------------------------------------- */

/* The groups of properties in bits (KR_MP_*) as the fields the phone knows;
 * everything when bits is ~0. */
static kr_packet *state_packet(const kr_player *p, unsigned bits, int64_t pos)
{
    kr_packet *k = kr_packet_new(MPRIS);
    if (!k)
        return NULL;
    cJSON *b = k->body;
    if (bits & KR_MP_VOLUME && p->volume >= 0)
        cJSON_AddNumberToObject(b, "volume", (int)(p->volume * 100));
    if (bits & KR_MP_META) {
        cJSON_AddStringToObject(b, "title", p->title);
        cJSON_AddStringToObject(b, "artist", p->artist);
        cJSON_AddStringToObject(b, "album", p->album);
        cJSON_AddStringToObject(b, "albumArtUrl", p->art_url);
        cJSON_AddNumberToObject(b, "length", (double)p->length_ms);
        cJSON_AddStringToObject(b, "url", p->url);
    }
    if (bits & KR_MP_STATUS)
        cJSON_AddBoolToObject(b, "isPlaying", strcmp(p->status, "Playing") == 0);
    if (bits & KR_MP_LOOP && p->has_loop)
        cJSON_AddStringToObject(b, "loopStatus", p->loop);
    if (bits & KR_MP_SHUFFLE && p->has_shuffle)
        cJSON_AddBoolToObject(b, "shuffle", p->shuffle);
    if (bits & KR_MP_CAN) {
        cJSON_AddBoolToObject(b, "canPause", p->can_pause);
        cJSON_AddBoolToObject(b, "canPlay", p->can_play);
        cJSON_AddBoolToObject(b, "canGoNext", p->can_next);
        cJSON_AddBoolToObject(b, "canGoPrevious", p->can_prev);
    }
    cJSON_AddStringToObject(b, "player", p->name);
    cJSON_AddBoolToObject(b, "canSeek", p->can_seek);
    if (p->can_seek && pos >= 0)
        cJSON_AddNumberToObject(b, "pos", (double)pos);
    return k;
}

/* A request in flight that needs the player's position first. */
typedef struct {
    kr_daemon *d;
    char device[64]; /* "" = every device */
    unsigned bits;
    bool volume;
} pending;

static void send_to(kr_daemon *d, const char *device, const kr_packet *k)
{
    if (device[0]) {
        kr_device *dev = kr_daemon_find_device(d, device);
        if (dev)
            kr_daemon_send(d, dev, k);
        return;
    }
    for (int i = 0; i < kr_daemon_device_count(d); i++)
        kr_daemon_send(d, kr_daemon_device_at(d, i), k);
}

static void on_position(void *ud, const char *player, int64_t pos)
{
    pending *pe = ud;
    kr_mpris *m = kr_daemon_mpris(pe->d);
    const kr_player *p = m ? kr_mpris_find(m, player) : NULL;
    if (p) {
        kr_packet *k = state_packet(p, pe->bits, pos);
        if (k) {
            if (pe->volume && p->volume >= 0 && !cJSON_HasObjectItem(k->body, "volume"))
                cJSON_AddNumberToObject(k->body, "volume", (int)(p->volume * 100));
            send_to(pe->d, pe->device, k);
            kr_packet_free(k);
        }
    }
    free(pe);
}

/* Sends the state (bits) of p to a device (or all), with a fresh position. */
static void send_state(kr_daemon *d, const kr_player *p, unsigned bits, const char *device, bool volume)
{
    kr_mpris *m = kr_daemon_mpris(d);
    pending *pe = calloc(1, sizeof *pe);
    if (!m || !pe) {
        free(pe);
        return;
    }
    pe->d = d;
    pe->bits = bits;
    pe->volume = volume;
    snprintf(pe->device, sizeof pe->device, "%s", device ? device : "");
    if (!p->can_seek || !kr_mpris_get_position(m, p, on_position, pe)) {
        on_position(pe, p->name, -1);
    }
}

/* ---- listener ---------------------------------------------------------------------- */

static void l_list(void *ud)
{
    send_list(ud, NULL);
}

static void l_change(void *ud, const kr_player *p, unsigned bits)
{
    send_state(ud, p, bits, NULL, false);
}

static void l_seeked(void *ud, const kr_player *p)
{
    kr_daemon *d = ud;
    kr_packet *k = kr_packet_new(MPRIS);
    if (!k)
        return;
    cJSON_AddNumberToObject(k->body, "pos", (double)p->pos_ms);
    cJSON_AddStringToObject(k->body, "player", p->name);
    send_to(d, "", k);
    kr_packet_free(k);
}

static void on_connect(kr_daemon *d, kr_device *dev)
{
    static bool listening;
    kr_mpris *m = kr_daemon_mpris(d);
    if (m && !listening) {
        kr_mpris_add_listener(m, &(kr_mpris_listener){l_list, l_change, l_seeked}, d);
        listening = true;
    }
    send_list(d, dev);
}

static void shutdown_plugin(kr_daemon *d)
{
    kr_mpris *m = kr_daemon_mpris(d);
    if (m)
        kr_mpris_remove_listener(m, d);
}

/* ---- album art ----------------------------------------------------------------------- */

/* file:///a/b%20c.jpg → /a/b c.jpg */
static bool file_url_path(const char *url, char *out, size_t size)
{
    if (strncmp(url, "file://", 7) != 0)
        return false;
    const char *s = url + 7;
    size_t n = 0;
    for (; *s && n + 1 < size; s++) {
        if (s[0] == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0}, *end;
            long v = strtol(hex, &end, 16);
            if (*end)
                return false;
            out[n++] = (char)v;
            s += 2;
        } else {
            out[n++] = *s;
        }
    }
    out[n] = '\0';
    return *s == '\0' && n > 0 && out[0] == '/';
}

static void send_art(kr_daemon *d, kr_device *dev, const kr_player *pl, const char *url)
{
    char path[KR_PATH_MAX];
    /* Only the art this player says it is showing, and only local files. */
    if (!pl->art_url[0] || strcmp(pl->art_url, url) != 0 || !file_url_path(url, path, sizeof path))
        return;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        if (fd >= 0)
            close(fd);
        return;
    }
    kr_packet *k = kr_packet_new(MPRIS);
    if (!k) {
        close(fd);
        return;
    }
    cJSON_AddBoolToObject(k->body, "transferringAlbumArt", true);
    cJSON_AddStringToObject(k->body, "player", pl->name);
    cJSON_AddStringToObject(k->body, "albumArtUrl", url);
    const char *slash = strrchr(path, '/');
    if (!kr_daemon_upload(d, dev, k, fd, st.st_size, slash ? slash + 1 : path))
        kr_log_debug("could not send the album art");
    kr_packet_free(k);
}

/* ---- requests from the phone ------------------------------------------------------------ */

typedef struct {
    kr_daemon *d;
    char device[64], player[160];
    int64_t target_ms;
} seek_to;

static void on_seek_position(void *ud, const char *player, int64_t pos)
{
    seek_to *s = ud;
    kr_mpris *m = kr_daemon_mpris(s->d);
    const kr_player *p = m ? kr_mpris_find(m, player) : NULL;
    if (p && pos >= 0)
        kr_mpris_seek(m, p, (s->target_ms - pos) * 1000);
    free(s);
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    if (cJSON_GetObjectItemCaseSensitive(p->body, "playerList"))
        return; /* from an MPRIS client like us, not a controller */
    kr_mpris *m = kr_daemon_mpris(d);
    const char *name = kr_packet_str(p, "player");
    const kr_player *pl = m && name ? kr_mpris_find(m, name) : NULL;
    if (!m)
        return;

    const char *art = kr_packet_str(p, "albumArtUrl");
    if (art) {
        if (pl)
            send_art(d, dev, pl, art);
        return;
    }
    if (!pl || kr_packet_bool(p, "requestPlayerList", false)) {
        send_list(d, dev);
        if (!pl)
            return;
    }

    const char *s;
    int64_t n;
    if ((s = kr_packet_str(p, "action")))
        kr_mpris_action(m, pl, s);
    if ((s = kr_packet_str(p, "setLoopStatus")))
        kr_mpris_set_loop(m, pl, s);
    if (cJSON_GetObjectItemCaseSensitive(p->body, "setShuffle"))
        kr_mpris_set_shuffle(m, pl, kr_packet_bool(p, "setShuffle", false));
    if (kr_packet_int(p, "setVolume", &n))
        kr_mpris_set_volume(m, pl, (double)n / 100.0);
    if (kr_packet_int(p, "Seek", &n))
        kr_mpris_seek(m, pl, n);
    if (kr_packet_int(p, "SetPosition", &n)) {
        seek_to *st = calloc(1, sizeof *st);
        if (st) {
            st->d = d;
            st->target_ms = n;
            if (!kr_mpris_get_position(m, pl, on_seek_position, st))
                free(st);
        }
    }

    bool now = kr_packet_bool(p, "requestNowPlaying", false);
    bool vol = kr_packet_bool(p, "requestVolume", false);
    if (now)
        send_state(d, pl, ~0u, kr_device_id(dev), vol);
    else if (vol && pl->volume >= 0) {
        kr_packet *k = kr_packet_new(MPRIS);
        if (k) {
            cJSON_AddNumberToObject(k->body, "volume", (int)(pl->volume * 100));
            cJSON_AddStringToObject(k->body, "player", pl->name);
            kr_daemon_send(d, dev, k);
            kr_packet_free(k);
        }
    }
}

const kr_plugin kr_plugin_mpris = {
    .name = "mpris",
    .incoming = incoming,
    .outgoing = outgoing,
    .shutdown = shutdown_plugin,
    .on_connect = on_connect,
    .on_packet = on_packet,
};
