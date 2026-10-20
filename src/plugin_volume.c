/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The phone controls the desktop's volume and output devices
 * (plugins/systemvolume upstream; and/plugins/systemvolume for the
 * client). Upstream talks to PulseAudio through libpulseaudioqt; here
 * everything goes through `pactl`, which PipeWire's pulse server answers
 * too, so the daemon needs no audio library.
 *
 * We take kdeconnect.systemvolume.request:
 *   {"requestSinks": true}                     the list of outputs
 *   {"name": "<sink>", "volume": n}            n in 0..maxVolume (65536 = 100%)
 *   {"name": "<sink>", "muted": bool}
 *   {"name": "<sink>", "enabled": true}        make it the default output
 * and send kdeconnect.systemvolume:
 *   {"sinkList": [{"name", "description", "volume", "maxVolume", "muted",
 *                  "enabled"}, ...]}
 *   {"name", "volume"} | {"name", "muted"} | {"name", "enabled"}  on changes
 *
 * The list is also sent on every connection (the Android app does not ask).
 * `pactl subscribe` runs only while a paired device is connected. Sink
 * names are only accepted if they were in the last list we read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "json.h"
#include "log.h"
#include "plugin.h"
#include "util.h"

#define REQUEST "kdeconnect.systemvolume.request"
#define VOLUME "kdeconnect.systemvolume"
#define MAX_VOLUME 65536 /* PulseAudioQt::normalVolume() */
#define SET_LIMIT 98304  /* 150%, what PulseAudio lets the volume go to */
#define DEBOUNCE_S 0.1
#define RESTART_S 2.0

static const char *const incoming[] = {REQUEST, NULL};
static const char *const outgoing[] = {VOLUME, NULL};

/* ---- parsing `pactl --format=json list sinks` ------------------------------- */

int kr_sinks_parse(const char *json, const char *default_name, kr_sink *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    int n = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, root)
    {
        if (n == max)
            break;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(s, "name");
        if (!cJSON_IsString(name) || !name->valuestring[0])
            continue;
        kr_sink *k = &out[n++];
        memset(k, 0, sizeof *k);
        snprintf(k->name, sizeof k->name, "%s", name->valuestring);
        /* "description" can be "(null)"; the device's own description
         * property is the next best, then the name. */
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(s, "description");
        const char *text = cJSON_IsString(desc) ? desc->valuestring : "";
        if (!text[0] || strcmp(text, "(null)") == 0) {
            const cJSON *props = cJSON_GetObjectItemCaseSensitive(s, "properties");
            const cJSON *pd = props ? cJSON_GetObjectItemCaseSensitive(props, "device.description") : NULL;
            text = cJSON_IsString(pd) && pd->valuestring[0] && strcmp(pd->valuestring, "(null)") != 0
                       ? pd->valuestring
                       : k->name;
        }
        snprintf(k->description, sizeof k->description, "%s", text);
        const cJSON *mute = cJSON_GetObjectItemCaseSensitive(s, "mute");
        k->muted = cJSON_IsTrue(mute);
        /* PulseAudio's volume of a sink is its loudest channel. */
        const cJSON *vol = cJSON_GetObjectItemCaseSensitive(s, "volume"), *ch;
        cJSON_ArrayForEach(ch, vol)
        {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(ch, "value");
            if (cJSON_IsNumber(v) && (int)v->valuedouble > k->volume)
                k->volume = (int)v->valuedouble;
        }
        k->enabled = default_name && strcmp(default_name, k->name) == 0;
    }
    cJSON_Delete(root);
    return n;
}

/* ---- the state ------------------------------------------------------------------- */

static struct {
    kr_sink sinks[KR_MAX_SINKS];
    int n;
    bool known;          /* sinks[] was read at least once */
    char default_name[256];
    int users;           /* paired devices connected */
    kr_proc *subscribe;
    kr_proc *refresh;    /* a read in progress */
    bool again;          /* ask once more when it finishes */
    bool send_full;      /* answer with the whole list when it finishes */
    double debounce_at, restart_at;
    bool source;
    bool no_pactl;
} st;

static bool pactl_ok(void)
{
    if (st.no_pactl)
        return false;
    if (!kr_which("pactl")) {
        kr_log_warn(_("pactl not found: the volume control is off"));
        st.no_pactl = true;
        return false;
    }
    return true;
}

static void pactl(kr_daemon *d, const char *a, const char *b, const char *c, const char *e)
{
    const char *argv[] = {"pactl", a, b, c, e, NULL};
    kr_proc_spawn(kr_daemon_procs(d), argv, false, NULL, NULL);
}

/* ---- packets ------------------------------------------------------------------------ */

static cJSON *sink_json(const kr_sink *k)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "name", k->name);
    cJSON_AddBoolToObject(o, "muted", k->muted);
    cJSON_AddStringToObject(o, "description", k->description);
    cJSON_AddNumberToObject(o, "volume", k->volume);
    cJSON_AddNumberToObject(o, "maxVolume", MAX_VOLUME);
    cJSON_AddBoolToObject(o, "enabled", k->enabled);
    return o;
}

static void broadcast(kr_daemon *d, const kr_packet *p)
{
    for (int i = 0; i < kr_daemon_device_count(d); i++)
        kr_daemon_send(d, kr_daemon_device_at(d, i), p);
}

static void send_list(kr_daemon *d)
{
    kr_packet *p = kr_packet_new(VOLUME);
    cJSON *a = p ? cJSON_AddArrayToObject(p->body, "sinkList") : NULL;
    if (a) {
        for (int i = 0; i < st.n; i++)
            cJSON_AddItemToArray(a, sink_json(&st.sinks[i]));
        broadcast(d, p);
    }
    kr_packet_free(p);
}

static void send_change(kr_daemon *d, const kr_sink *k, const char *field)
{
    kr_packet *p = kr_packet_new(VOLUME);
    if (!p)
        return;
    if (strcmp(field, "volume") == 0)
        cJSON_AddNumberToObject(p->body, "volume", k->volume);
    else if (strcmp(field, "muted") == 0)
        cJSON_AddBoolToObject(p->body, "muted", k->muted);
    else
        cJSON_AddBoolToObject(p->body, "enabled", k->enabled);
    cJSON_AddStringToObject(p->body, "name", k->name);
    broadcast(d, p);
    kr_packet_free(p);
}

/* New list read: tell the phones what changed. */
static void apply(kr_daemon *d, const kr_sink *now, int n)
{
    bool same_set = st.known && n == st.n;
    for (int i = 0; same_set && i < n; i++)
        same_set = strcmp(now[i].name, st.sinks[i].name) == 0;
    kr_sink old[KR_MAX_SINKS];
    int old_n = st.n;
    memcpy(old, st.sinks, sizeof old);
    memcpy(st.sinks, now, (size_t)n * sizeof *now);
    st.n = n;
    st.known = true;

    if (!same_set || st.send_full) {
        st.send_full = false;
        send_list(d);
        return;
    }
    for (int i = 0; i < n && i < old_n; i++) {
        if (now[i].volume != old[i].volume)
            send_change(d, &now[i], "volume");
        if (now[i].muted != old[i].muted)
            send_change(d, &now[i], "muted");
        if (now[i].enabled != old[i].enabled)
            send_change(d, &now[i], "enabled");
    }
}

/* ---- reading ------------------------------------------------------------------------- */

static void start_refresh(kr_daemon *d);

static void on_list(void *ud, int status, const char *out, size_t len)
{
    (void)len;
    kr_daemon *d = ud;
    st.refresh = NULL;
    if (status == 0 && out) {
        kr_sink now[KR_MAX_SINKS];
        int n = kr_sinks_parse(out, st.default_name, now, KR_MAX_SINKS);
        apply(d, now, n);
    }
    if (st.again) {
        st.again = false;
        start_refresh(d);
    }
}

static void on_info(void *ud, int status, const char *out, size_t len)
{
    (void)len;
    kr_daemon *d = ud;
    st.refresh = NULL;
    st.default_name[0] = '\0';
    cJSON *j = status == 0 && out ? cJSON_Parse(out) : NULL;
    const char *def = kr_json_str(j, "default_sink_name");
    if (def)
        snprintf(st.default_name, sizeof st.default_name, "%s", def);
    cJSON_Delete(j);
    const char *argv[] = {"pactl", "--format=json", "list", "sinks", NULL};
    st.refresh = kr_proc_spawn(kr_daemon_procs(d), argv, true, &(kr_proc_cb){.on_exit = on_list}, d);
    if (!st.refresh && st.again) {
        st.again = false;
        start_refresh(d);
    }
}

/* Reads the default sink, then the sinks. One read at a time. */
static void start_refresh(kr_daemon *d)
{
    if (st.refresh) {
        st.again = true;
        return;
    }
    const char *argv[] = {"pactl", "--format=json", "info", NULL};
    st.refresh = kr_proc_spawn(kr_daemon_procs(d), argv, true, &(kr_proc_cb){.on_exit = on_info}, d);
}

/* ---- watching ------------------------------------------------------------------------ */

static void on_event(void *ud, const char *line)
{
    kr_daemon *d = ud;
    (void)d;
    /* "Event 'change' on sink #5" / "... on server" */
    if (strstr(line, " on sink ") || strstr(line, " on server")) {
        if (!st.debounce_at)
            st.debounce_at = kr_now() + DEBOUNCE_S;
    }
}

static void on_subscribe_exit(void *ud, int status, const char *out, size_t len)
{
    (void)ud;
    (void)status;
    (void)out;
    (void)len;
    st.subscribe = NULL;
    if (st.users > 0)
        st.restart_at = kr_now() + RESTART_S;
}

static void start_subscribe(kr_daemon *d)
{
    if (st.subscribe)
        return;
    const char *argv[] = {"pactl", "subscribe", NULL};
    st.subscribe = kr_proc_spawn(kr_daemon_procs(d), argv, false,
                                 &(kr_proc_cb){.on_line = on_event, .on_exit = on_subscribe_exit}, d);
    if (!st.subscribe)
        st.restart_at = kr_now() + RESTART_S;
}

static double src_deadline(void *ud)
{
    (void)ud;
    double a = st.debounce_at, b = st.restart_at;
    return a && b ? (a < b ? a : b) : (a ? a : b);
}

static void src_dispatch(void *ud, const struct pollfd *fds, int n)
{
    (void)fds;
    (void)n;
    kr_daemon *d = ud;
    double now = kr_now();
    if (st.debounce_at && now >= st.debounce_at) {
        st.debounce_at = 0;
        if (st.users > 0)
            start_refresh(d);
    }
    if (st.restart_at && now >= st.restart_at) {
        st.restart_at = 0;
        if (st.users > 0)
            start_subscribe(d);
    }
}

static void on_connect(kr_daemon *d, kr_device *dev)
{
    (void)dev;
    if (!pactl_ok())
        return;
    if (!st.source)
        st.source = kr_daemon_add_source(d, &(kr_source){NULL, src_dispatch, src_deadline}, d);
    if (st.users++ == 0)
        start_subscribe(d);
    /* The list goes to the phone as soon as it is read, without being asked:
     * the Android app never sends requestSinks (the upstream desktop sends
     * the list from connected()). Without send_full, a second connection
     * would see an unchanged list and get nothing. */
    st.send_full = true;
    start_refresh(d);
}

static void on_disconnect(kr_daemon *d, kr_device *dev)
{
    (void)d;
    (void)dev;
    if (st.no_pactl || st.users == 0)
        return;
    if (--st.users == 0) {
        if (st.subscribe) {
            kr_proc_detach(st.subscribe);
            kr_proc_kill(st.subscribe);
            st.subscribe = NULL;
        }
        st.debounce_at = st.restart_at = 0;
    }
}

static void shutdown_plugin(kr_daemon *d)
{
    if (st.refresh)
        kr_proc_detach(st.refresh);
    if (st.subscribe) {
        kr_proc_detach(st.subscribe);
        kr_proc_kill(st.subscribe);
    }
    if (st.source)
        kr_daemon_remove_source(d, d);
    memset(&st, 0, sizeof st);
}

/* ---- requests from the phone ------------------------------------------------------------ */

static const kr_sink *known_sink(const char *name)
{
    for (int i = 0; name && i < st.n; i++)
        if (strcmp(st.sinks[i].name, name) == 0)
            return &st.sinks[i];
    return NULL;
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    (void)dev;
    if (st.no_pactl)
        return;
    if (cJSON_GetObjectItemCaseSensitive(p->body, "requestSinks")) {
        st.send_full = true;
        start_refresh(d);
        return;
    }
    const kr_sink *k = known_sink(kr_packet_str(p, "name"));
    if (!k) {
        kr_log_debug("volume request for an unknown output");
        return;
    }
    int64_t v;
    if (kr_packet_int(p, "volume", &v)) {
        char num[16];
        snprintf(num, sizeof num, "%d", v < 0 ? 0 : v > SET_LIMIT ? SET_LIMIT : (int)v);
        pactl(d, "set-sink-volume", k->name, num, NULL);
        pactl(d, "set-sink-mute", k->name, "0", NULL); /* upstream unmutes when the volume is set */
    }
    if (cJSON_GetObjectItemCaseSensitive(p->body, "muted"))
        pactl(d, "set-sink-mute", k->name, kr_packet_bool(p, "muted", false) ? "1" : "0", NULL);
    if (kr_packet_bool(p, "enabled", false))
        pactl(d, "set-default-sink", k->name, NULL, NULL);
}

const kr_plugin kr_plugin_volume = {
    .name = "systemvolume",
    .incoming = incoming,
    .outgoing = outgoing,
    .shutdown = shutdown_plugin,
    .on_connect = on_connect,
    .on_disconnect = on_disconnect,
    .on_packet = on_packet,
};
