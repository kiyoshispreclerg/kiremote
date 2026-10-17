/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mpris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#define PREFIX "org.mpris.MediaPlayer2."
#define PATH "/org/mpris/MediaPlayer2"
#define ROOT_IFACE "org.mpris.MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define MAX_PLAYERS 16
#define MAX_LISTENERS 4
#define TIMEOUT_MS 2000

typedef struct {
    kr_player pub;
    int waiting;        /* replies still expected before it is loaded */
    char identity[128];
} player;

typedef struct {
    kr_mpris_listener l;
    void *ud;
} listener;

struct kr_mpris {
    kr_bus *bus;
    player *players[MAX_PLAYERS];
    int n_players;
    listener listeners[MAX_LISTENERS];
    int n_listeners;
};

/* Context of a call in flight; the player is looked up again on the reply. */
typedef struct {
    kr_mpris *m;
    char service[128];
    kr_mpris_pos_fn pos_cb;
    void *pos_ud;
} ctx;

/* ---- variants ------------------------------------------------------------------- */

static bool get_int64(DBusMessageIter *v, int64_t *out)
{
    switch (dbus_message_iter_get_arg_type(v)) {
    case DBUS_TYPE_INT64: {
        dbus_int64_t x;
        dbus_message_iter_get_basic(v, &x);
        *out = x;
        return true;
    }
    case DBUS_TYPE_UINT64: {
        dbus_uint64_t x;
        dbus_message_iter_get_basic(v, &x);
        *out = (int64_t)x;
        return true;
    }
    case DBUS_TYPE_INT32: {
        dbus_int32_t x;
        dbus_message_iter_get_basic(v, &x);
        *out = x;
        return true;
    }
    case DBUS_TYPE_UINT32: {
        dbus_uint32_t x;
        dbus_message_iter_get_basic(v, &x);
        *out = x;
        return true;
    }
    default:
        return false;
    }
}

static bool get_double(DBusMessageIter *v, double *out)
{
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_DOUBLE) {
        dbus_message_iter_get_basic(v, out);
        return true;
    }
    int64_t i;
    if (get_int64(v, &i)) {
        *out = (double)i;
        return true;
    }
    return false;
}

static bool get_bool(DBusMessageIter *v, bool *out)
{
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_BOOLEAN)
        return false;
    dbus_bool_t b;
    dbus_message_iter_get_basic(v, &b);
    *out = b;
    return true;
}

static bool get_string(DBusMessageIter *v, char *out, size_t size)
{
    int t = dbus_message_iter_get_arg_type(v);
    if (t != DBUS_TYPE_STRING && t != DBUS_TYPE_OBJECT_PATH)
        return false;
    const char *s;
    dbus_message_iter_get_basic(v, &s);
    snprintf(out, size, "%s", s);
    return true;
}

/* An array of strings joined with ", " (xesam:artist). */
static void get_string_list(DBusMessageIter *v, char *out, size_t size)
{
    out[0] = '\0';
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_STRING) {
        get_string(v, out, size);
        return;
    }
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter arr;
    dbus_message_iter_recurse(v, &arr);
    size_t len = 0;
    for (; dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING; dbus_message_iter_next(&arr)) {
        const char *s;
        dbus_message_iter_get_basic(&arr, &s);
        int n = snprintf(out + len, size - len, "%s%s", len ? ", " : "", s);
        if (n < 0 || (size_t)n >= size - len)
            break;
        len += (size_t)n;
    }
}

/* ---- properties --------------------------------------------------------------------- */

static void parse_metadata(kr_player *p, DBusMessageIter *dict)
{
    p->title[0] = p->artist[0] = p->album[0] = p->art_url[0] = p->url[0] = '\0';
    p->length_ms = -1;
    DBusMessageIter e;
    dbus_message_iter_recurse(dict, &e);
    for (; dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&e)) {
        DBusMessageIter kv, var;
        const char *key;
        dbus_message_iter_recurse(&e, &kv);
        dbus_message_iter_get_basic(&kv, &key);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &var);
        int64_t n;
        if (strcmp(key, "xesam:title") == 0)
            get_string(&var, p->title, sizeof p->title);
        else if (strcmp(key, "xesam:artist") == 0)
            get_string_list(&var, p->artist, sizeof p->artist);
        else if (strcmp(key, "xesam:album") == 0)
            get_string(&var, p->album, sizeof p->album);
        else if (strcmp(key, "mpris:artUrl") == 0)
            get_string(&var, p->art_url, sizeof p->art_url);
        else if (strcmp(key, "xesam:url") == 0)
            get_string(&var, p->url, sizeof p->url);
        else if (strcmp(key, "mpris:length") == 0 && get_int64(&var, &n))
            p->length_ms = n / 1000; /* µs to ms */
    }
    /* A local file without tags: the file name, in its folder's album, as
     * the upstream does. */
    if (!p->title[0] && !p->artist[0] && strncmp(p->url, "file://", 7) == 0) {
        const char *path = p->url + 7, *slash = strrchr(path, '/');
        snprintf(p->title, sizeof p->title, "%s", slash ? slash + 1 : path);
        if (!p->album[0] && slash && slash > path) {
            const char *start = slash;
            while (start > path && start[-1] != '/')
                start--;
            snprintf(p->album, sizeof p->album, "%.*s", (int)(slash - start), start);
        }
    }
}

/* One property of the Player interface; returns the KR_MP_* bit it changed. */
static unsigned apply_prop(kr_player *p, const char *key, DBusMessageIter *var)
{
    bool b;
    double x;
    if (strcmp(key, "PlaybackStatus") == 0 && get_string(var, p->status, sizeof p->status))
        return KR_MP_STATUS;
    if (strcmp(key, "Metadata") == 0 && dbus_message_iter_get_arg_type(var) == DBUS_TYPE_ARRAY) {
        parse_metadata(p, var);
        return KR_MP_META;
    }
    if (strcmp(key, "Volume") == 0 && get_double(var, &x)) {
        p->volume = x;
        return KR_MP_VOLUME;
    }
    if (strcmp(key, "LoopStatus") == 0 && get_string(var, p->loop, sizeof p->loop)) {
        p->has_loop = true;
        return KR_MP_LOOP;
    }
    if (strcmp(key, "Shuffle") == 0 && get_bool(var, &b)) {
        p->shuffle = b;
        p->has_shuffle = true;
        return KR_MP_SHUFFLE;
    }
    if (strcmp(key, "Position") == 0 && get_double(var, &x)) {
        p->pos_ms = (int64_t)(x / 1000);
        return 0;
    }
    struct {
        const char *key;
        bool *field;
    } can[] = {{"CanPause", &p->can_pause}, {"CanPlay", &p->can_play}, {"CanGoNext", &p->can_next},
               {"CanGoPrevious", &p->can_prev}, {"CanSeek", &p->can_seek}};
    for (size_t i = 0; i < sizeof can / sizeof *can; i++)
        if (strcmp(key, can[i].key) == 0 && get_bool(var, &b)) {
            *can[i].field = b;
            return KR_MP_CAN;
        }
    return 0;
}

/* A dict a{sv} of properties; returns the bits that changed. root: it is
 * the interface org.mpris.MediaPlayer2 (Identity), not the Player one. */
static unsigned apply_dict(player *pl, DBusMessageIter *arr, bool root)
{
    unsigned bits = 0;
    DBusMessageIter e;
    dbus_message_iter_recurse(arr, &e);
    for (; dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&e)) {
        DBusMessageIter kv, var;
        const char *key;
        dbus_message_iter_recurse(&e, &kv);
        dbus_message_iter_get_basic(&kv, &key);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &var);
        if (root) {
            if (strcmp(key, "Identity") == 0)
                get_string(&var, pl->identity, sizeof pl->identity);
        } else {
            bits |= apply_prop(&pl->pub, key, &var);
        }
    }
    return bits;
}

/* ---- players ----------------------------------------------------------------------- */

static player *by_service(kr_mpris *m, const char *service)
{
    for (int i = 0; i < m->n_players; i++)
        if (strcmp(m->players[i]->pub.service, service) == 0)
            return m->players[i];
    return NULL;
}

static player *by_owner(kr_mpris *m, const char *owner)
{
    for (int i = 0; owner && i < m->n_players; i++)
        if (m->players[i]->pub.owner[0] && strcmp(m->players[i]->pub.owner, owner) == 0)
            return m->players[i];
    return NULL;
}

static void notify_list(kr_mpris *m)
{
    for (int i = 0; i < m->n_listeners; i++)
        if (m->listeners[i].l.on_list)
            m->listeners[i].l.on_list(m->listeners[i].ud);
}

/* The Identity, made unique among the loaded players: "VLC [2]". */
static void assign_name(kr_mpris *m, player *pl)
{
    const char *base = pl->identity[0] ? pl->identity : pl->pub.service + sizeof PREFIX - 1;
    char name[160];
    snprintf(name, sizeof name, "%s", base);
    for (int n = 2;; n++) {
        bool taken = false;
        for (int i = 0; i < m->n_players; i++)
            taken |= m->players[i] != pl && m->players[i]->pub.loaded &&
                     strcmp(m->players[i]->pub.name, name) == 0;
        if (!taken)
            break;
        snprintf(name, sizeof name, "%.140s [%d]", base, n);
    }
    snprintf(pl->pub.name, sizeof pl->pub.name, "%s", name);
}

static void part_loaded(kr_mpris *m, player *pl)
{
    if (--pl->waiting > 0)
        return;
    pl->pub.loaded = true;
    assign_name(m, pl);
    kr_log_debug("MPRIS player %s (%s)", pl->pub.name, pl->pub.service);
    notify_list(m);
}

static ctx *new_ctx(kr_mpris *m, const char *service)
{
    ctx *c = calloc(1, sizeof *c);
    if (c) {
        c->m = m;
        snprintf(c->service, sizeof c->service, "%s", service);
    }
    return c;
}

static void on_getall(void *ud, DBusMessage *reply, bool root)
{
    ctx *c = ud;
    player *pl = by_service(c->m, c->service);
    DBusMessageIter it;
    if (pl && reply && dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY)
        apply_dict(pl, &it, root);
    if (pl)
        part_loaded(c->m, pl); /* even when it failed: show what we have */
    free(c);
}

static void on_getall_root(void *ud, DBusMessage *reply)
{
    on_getall(ud, reply, true);
}

static void on_getall_player(void *ud, DBusMessage *reply)
{
    on_getall(ud, reply, false);
}

static void on_owner(void *ud, DBusMessage *reply)
{
    ctx *c = ud;
    player *pl = by_service(c->m, c->service);
    const char *owner = NULL;
    if (pl && reply)
        dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID);
    if (pl && owner)
        snprintf(pl->pub.owner, sizeof pl->pub.owner, "%s", owner);
    if (pl)
        part_loaded(c->m, pl);
    free(c);
}

/* Sends msg (consumed) with a context for the reply. */
static void ask(kr_mpris *m, DBusMessage *msg, kr_bus_reply_fn fn, const char *service)
{
    if (!msg)
        return;
    ctx *c = new_ctx(m, service);
    if (!c)
        dbus_message_unref(msg);
    else if (!kr_bus_call(m->bus, msg, fn, c, TIMEOUT_MS))
        free(c);
}

static void get_all(kr_mpris *m, const char *service, const char *iface, kr_bus_reply_fn fn)
{
    DBusMessage *msg = kr_bus_new_call(service, PATH, PROPS_IFACE, "GetAll");
    if (msg && !dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        msg = NULL;
    }
    ask(m, msg, fn, service);
}

static bool wanted(const char *service)
{
    return strncmp(service, PREFIX, sizeof PREFIX - 1) == 0 &&
           strncmp(service, PREFIX "kdeconnect.", strlen(PREFIX "kdeconnect.")) != 0 &&
           strcmp(service, PREFIX "playerctld") != 0;
}

/* Starts tracking a player; owner may be NULL (then it is asked for). */
static void track(kr_mpris *m, const char *service, const char *owner)
{
    if (!wanted(service) || by_service(m, service) || m->n_players == MAX_PLAYERS)
        return;
    player *pl = calloc(1, sizeof *pl);
    if (!pl)
        return;
    snprintf(pl->pub.service, sizeof pl->pub.service, "%s", service);
    pl->pub.volume = -1;
    pl->pub.length_ms = -1;
    snprintf(pl->pub.status, sizeof pl->pub.status, "Stopped");
    pl->waiting = owner ? 2 : 3;
    if (owner)
        snprintf(pl->pub.owner, sizeof pl->pub.owner, "%s", owner);
    m->players[m->n_players++] = pl;
    if (!owner) {
        DBusMessage *msg = kr_bus_new_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner");
        if (msg && !dbus_message_append_args(msg, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID)) {
            dbus_message_unref(msg);
            msg = NULL;
        }
        ask(m, msg, on_owner, service);
    }
    get_all(m, service, ROOT_IFACE, on_getall_root);
    get_all(m, service, PLAYER_IFACE, on_getall_player);
}

static void untrack(kr_mpris *m, const char *service)
{
    player *pl = by_service(m, service);
    if (!pl)
        return;
    bool was = pl->pub.loaded;
    for (int i = 0; i < m->n_players; i++)
        if (m->players[i] == pl) {
            m->players[i] = m->players[--m->n_players];
            break;
        }
    free(pl);
    if (was)
        notify_list(m);
}

static void on_list_names(void *ud, DBusMessage *reply)
{
    kr_mpris *m = ud;
    DBusMessageIter it, arr;
    if (!reply || !dbus_message_iter_init(reply, &it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
        return;
    dbus_message_iter_recurse(&it, &arr);
    for (; dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING; dbus_message_iter_next(&arr)) {
        const char *name;
        dbus_message_iter_get_basic(&arr, &name);
        track(m, name, NULL);
    }
}

void kr_mpris_handle_signal(kr_mpris *m, DBusMessage *msg)
{
    if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        const char *name, *old, *new_owner;
        if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old,
                                   DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) ||
            !wanted(name))
            return;
        if (*old || !*new_owner)
            untrack(m, name);
        if (*new_owner)
            track(m, name, new_owner);
        return;
    }
    if (dbus_message_is_signal(msg, PROPS_IFACE, "PropertiesChanged")) {
        player *pl = by_owner(m, dbus_message_get_sender(msg));
        DBusMessageIter it;
        const char *iface;
        if (!pl || !dbus_message_iter_init(msg, &it) ||
            dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
            return;
        dbus_message_iter_get_basic(&it, &iface);
        if (strcmp(iface, PLAYER_IFACE) != 0 || !dbus_message_iter_next(&it) ||
            dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
            return;
        unsigned bits = apply_dict(pl, &it, false);
        if (bits && pl->pub.loaded)
            for (int i = 0; i < m->n_listeners; i++)
                if (m->listeners[i].l.on_change)
                    m->listeners[i].l.on_change(m->listeners[i].ud, &pl->pub, bits);
        return;
    }
    if (dbus_message_is_signal(msg, PLAYER_IFACE, "Seeked")) {
        player *pl = by_owner(m, dbus_message_get_sender(msg));
        dbus_int64_t pos;
        if (!pl || !dbus_message_get_args(msg, NULL, DBUS_TYPE_INT64, &pos, DBUS_TYPE_INVALID))
            return;
        pl->pub.pos_ms = pos / 1000;
        if (pl->pub.loaded)
            for (int i = 0; i < m->n_listeners; i++)
                if (m->listeners[i].l.on_seeked)
                    m->listeners[i].l.on_seeked(m->listeners[i].ud, &pl->pub);
    }
}

static void on_signal(void *ud, DBusMessage *msg)
{
    kr_mpris_handle_signal(ud, msg);
}

kr_mpris *kr_mpris_new(kr_bus *bus)
{
    kr_mpris *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->bus = bus;
    if (kr_bus_alive(bus)) {
        kr_bus_listen(bus,
                      "type='signal',sender='" DBUS_SERVICE_DBUS "',interface='" DBUS_INTERFACE_DBUS
                      "',member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'",
                      on_signal, m);
        kr_bus_listen(bus,
                      "type='signal',interface='" PROPS_IFACE "',member='PropertiesChanged',path='" PATH "'",
                      on_signal, m);
        kr_bus_listen(bus, "type='signal',interface='" PLAYER_IFACE "',member='Seeked',path='" PATH "'",
                      on_signal, m);
        kr_bus_call(bus, kr_bus_new_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "ListNames"),
                    on_list_names, m, TIMEOUT_MS);
    }
    return m;
}

void kr_mpris_free(kr_mpris *m)
{
    if (!m)
        return;
    if (kr_bus_alive(m->bus))
        kr_bus_unlisten(m->bus, on_signal, m);
    for (int i = 0; i < m->n_players; i++)
        free(m->players[i]);
    free(m);
}

void kr_mpris_add_listener(kr_mpris *m, const kr_mpris_listener *l, void *ud)
{
    if (m->n_listeners < MAX_LISTENERS)
        m->listeners[m->n_listeners++] = (listener){*l, ud};
}

void kr_mpris_remove_listener(kr_mpris *m, void *ud)
{
    for (int i = 0; i < m->n_listeners;) {
        if (m->listeners[i].ud == ud)
            m->listeners[i] = m->listeners[--m->n_listeners];
        else
            i++;
    }
}

int kr_mpris_count(const kr_mpris *m)
{
    int n = 0;
    for (int i = 0; i < m->n_players; i++)
        n += m->players[i]->pub.loaded;
    return n;
}

const kr_player *kr_mpris_at(const kr_mpris *m, int k)
{
    for (int i = 0; i < m->n_players; i++)
        if (m->players[i]->pub.loaded && k-- == 0)
            return &m->players[i]->pub;
    return NULL;
}

const kr_player *kr_mpris_find(const kr_mpris *m, const char *name)
{
    for (int i = 0; i < m->n_players; i++)
        if (m->players[i]->pub.loaded && strcmp(m->players[i]->pub.name, name) == 0)
            return &m->players[i]->pub;
    return NULL;
}

/* ---- driving ------------------------------------------------------------------------ */

bool kr_mpris_action(kr_mpris *m, const kr_player *p, const char *action)
{
    static const char *const ok[] = {"Play", "Pause", "PlayPause", "Stop", "Next", "Previous"};
    bool known = false;
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
        known |= strcmp(action, ok[i]) == 0;
    if (!known)
        return false;
    return kr_bus_call(m->bus, kr_bus_new_call(p->service, PATH, PLAYER_IFACE, action), NULL, NULL, 0);
}

/* Properties.Set(Player, key, variant<sig>(value)) */
static bool set_prop(kr_mpris *m, const kr_player *p, const char *key, int type, const char *sig,
                     const void *value)
{
    DBusMessage *msg = kr_bus_new_call(p->service, PATH, PROPS_IFACE, "Set");
    if (!msg)
        return false;
    const char *iface = PLAYER_IFACE;
    DBusMessageIter it, var;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &var);
    dbus_message_iter_append_basic(&var, type, value);
    dbus_message_iter_close_container(&it, &var);
    return kr_bus_call(m->bus, msg, NULL, NULL, 0);
}

bool kr_mpris_set_volume(kr_mpris *m, const kr_player *p, double volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > 1)
        volume = 1;
    return set_prop(m, p, "Volume", DBUS_TYPE_DOUBLE, "d", &volume);
}

bool kr_mpris_set_loop(kr_mpris *m, const kr_player *p, const char *loop)
{
    if (strcmp(loop, "None") != 0 && strcmp(loop, "Track") != 0 && strcmp(loop, "Playlist") != 0)
        return false;
    return set_prop(m, p, "LoopStatus", DBUS_TYPE_STRING, "s", &loop);
}

bool kr_mpris_set_shuffle(kr_mpris *m, const kr_player *p, bool on)
{
    dbus_bool_t v = on;
    return set_prop(m, p, "Shuffle", DBUS_TYPE_BOOLEAN, "b", &v);
}

bool kr_mpris_seek(kr_mpris *m, const kr_player *p, int64_t offset_us)
{
    DBusMessage *msg = kr_bus_new_call(p->service, PATH, PLAYER_IFACE, "Seek");
    dbus_int64_t off = offset_us;
    if (!msg || !dbus_message_append_args(msg, DBUS_TYPE_INT64, &off, DBUS_TYPE_INVALID)) {
        if (msg)
            dbus_message_unref(msg);
        return false;
    }
    return kr_bus_call(m->bus, msg, NULL, NULL, 0);
}

static void on_position(void *ud, DBusMessage *reply)
{
    ctx *c = ud;
    int64_t pos = -1;
    DBusMessageIter it, var;
    if (reply && dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &var);
        int64_t us;
        if (get_int64(&var, &us))
            pos = us / 1000;
    }
    player *pl = by_service(c->m, c->service);
    if (pl && pos >= 0)
        pl->pub.pos_ms = pos;
    c->pos_cb(c->pos_ud, pl ? pl->pub.name : "", pos);
    free(c);
}

bool kr_mpris_get_position(kr_mpris *m, const kr_player *p, kr_mpris_pos_fn cb, void *ud)
{
    DBusMessage *msg = kr_bus_new_call(p->service, PATH, PROPS_IFACE, "Get");
    ctx *c = new_ctx(m, p->service);
    const char *iface = PLAYER_IFACE, *key = "Position";
    if (msg && c && dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &key,
                                             DBUS_TYPE_INVALID)) {
        c->pos_cb = cb;
        c->pos_ud = ud;
        if (kr_bus_call(m->bus, msg, on_position, c, TIMEOUT_MS))
            return true;
        free(c);
        return false;
    }
    if (msg)
        dbus_message_unref(msg);
    free(c);
    return false;
}
