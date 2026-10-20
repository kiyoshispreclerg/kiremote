/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The phone's notifications on the desktop (plugins/notifications upstream
 * and its README; and/plugins/notifications for the sender):
 *
 *   in  kdeconnect.notification   {"id", "appName", "title", "text",
 *         "ticker", "isClearable", "isCancel", "silent", "actions": [..],
 *         "requestReplyId", "conversation": [{"sender","content"}],
 *         "payloadHash"} + the icon (PNG) as a payload when payloadHash is
 *         there. The same "id" again updates the notification; isCancel
 *         means it was dismissed on the phone.
 *   out kdeconnect.notification.request  {"request": true}  on connection,
 *         {"cancel": "<id>"} when the user dismisses one here
 *   out kdeconnect.notification.action   {"key": "<id>", "action": "<label>"}
 *
 * Replying from the notification (kdeconnect.notification.reply) needs an
 * inline-reply field the notification servers here don't have; not yet.
 * A notification the phone marks "silent" (the ones that already existed
 * when we connected) is not shown. Icons are cached under
 * $XDG_CACHE_HOME/kiremote/icons/<payloadHash>. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "util.h"

#define NOTIFICATION "kdeconnect.notification"
#define REQUEST "kdeconnect.notification.request"
#define ACTION "kdeconnect.notification.action"
#define MAX_ENTRIES 64
#define MAX_ACTIONS 5
#define MAX_ICON (1u << 20)
#define ID_MAX 192
#define ID_PREFIX "org.kde.kdeconnect_tp::"

static const char *const incoming[] = {NOTIFICATION, NULL};
static const char *const outgoing[] = {REQUEST, ACTION, NULL};

struct state;

typedef struct {
    struct state *st;
    char id[ID_MAX];
    bool clearable;
    char *summary, *body;
    char *actions[MAX_ACTIONS]; /* labels, as the phone sent them */
    int n_actions;
    char icon[KR_PATH_MAX]; /* "" until known */
    bool silent, icon_pending;
    kr_note *note;
} entry;

typedef struct state {
    kr_daemon *d;
    char dev_id[64];
    entry *v[MAX_ENTRIES];
    int n;
} state;

/* ---- entries ---------------------------------------------------------------- */

static void free_entry_text(entry *e)
{
    free(e->summary);
    free(e->body);
    for (int i = 0; i < e->n_actions; i++)
        free(e->actions[i]);
    e->summary = e->body = NULL;
    e->n_actions = 0;
}

static entry *find_entry(state *st, const char *id)
{
    for (int i = 0; i < st->n; i++)
        if (strcmp(st->v[i]->id, id) == 0)
            return st->v[i];
    return NULL;
}

static void drop_entry(state *st, entry *e, bool close_note)
{
    if (e->note) {
        kr_note_detach(e->note);
        if (close_note)
            kr_note_close(e->note);
    }
    for (int i = 0; i < st->n; i++)
        if (st->v[i] == e) {
            st->v[i] = st->v[--st->n];
            break;
        }
    free_entry_text(e);
    free(e);
}

/* ---- sending to the phone ---------------------------------------------------------- */

static void send_to_phone(state *st, const char *type, const char *k1, const char *v1, const char *k2,
                          const char *v2)
{
    kr_device *dev = kr_daemon_find_device(st->d, st->dev_id);
    kr_packet *p = dev ? kr_packet_new(type) : NULL;
    if (!p)
        return;
    cJSON_AddStringToObject(p->body, k1, v1);
    if (k2)
        cJSON_AddStringToObject(p->body, k2, v2);
    kr_daemon_send(st->d, dev, p);
    kr_packet_free(p);
}

/* The upstream copies an auth code out of an action's label ("Copy 123456"):
 * a run of 4 to 10 digits standing alone, ignoring U+2063 between digits. */
static void copy_auth_code(kr_daemon *d, const char *label)
{
    char clean[256];
    size_t o = 0;
    for (const char *c = label; *c && o + 1 < sizeof clean; c++) {
        if ((unsigned char)c[0] == 0xE2 && (unsigned char)c[1] == 0x81 && (unsigned char)c[2] == 0xA3) {
            c += 2; /* U+2063 INVISIBLE SEPARATOR */
            continue;
        }
        clean[o++] = *c;
    }
    clean[o] = '\0';
    for (size_t i = 0; clean[i];) {
        if (!isdigit((unsigned char)clean[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (isdigit((unsigned char)clean[j]))
            j++;
        bool alone = (i == 0 || !isalnum((unsigned char)clean[i - 1])) && !isalnum((unsigned char)clean[j]);
        if (alone && j - i >= 4 && j - i <= 10) {
            char code[16];
            snprintf(code, sizeof code, "%.*s", (int)(j - i), clean + i);
            kr_clipboard_set_text(d, code);
            return;
        }
        i = j;
    }
}

/* ---- showing ----------------------------------------------------------------------- */

static void on_note_action(void *ud, const char *key)
{
    entry *e = ud;
    int i = key[0] == 'a' ? atoi(key + 1) : -1;
    if (i < 0 || i >= e->n_actions)
        return;
    send_to_phone(e->st, ACTION, "key", e->id, "action", e->actions[i]);
    copy_auth_code(e->st->d, e->actions[i]);
}

static void on_note_closed(void *ud, int reason)
{
    entry *e = ud;
    e->note = NULL;
    /* The user closed it here: dismiss it on the phone too, if it lets us. */
    if (reason == KR_NOTE_DISMISSED && e->clearable) {
        send_to_phone(e->st, REQUEST, "cancel", e->id, NULL, NULL);
        drop_entry(e->st, e, false);
    }
}

static void present(entry *e)
{
    if (e->silent || e->icon_pending)
        return;
    kr_notify *n = kr_daemon_notify(e->st->d);
    const char *acts[2 * MAX_ACTIONS + 1];
    char keys[MAX_ACTIONS][16];
    int k = 0;
    for (int i = 0; i < e->n_actions; i++) {
        snprintf(keys[i], sizeof keys[i], "a%d", i);
        acts[k++] = keys[i];
        acts[k++] = e->actions[i];
    }
    acts[k] = NULL;
    kr_note_spec spec = {.summary = e->summary,
                         .body = e->body,
                         .icon = e->icon[0] ? e->icon : "phone",
                         .actions = k ? acts : NULL,
                         .timeout_ms = -1};
    if (e->note && kr_note_update(e->note, &spec))
        return;
    if (e->note) {
        kr_note_detach(e->note);
        kr_note_close(e->note);
    }
    e->note = kr_notify_show(n, &spec, &(kr_note_cb){.on_action = on_note_action, .on_closed = on_note_closed}, e);
}

/* ---- icons ------------------------------------------------------------------------ */

static bool hash_ok(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 64)
        return false;
    for (; *h; h++)
        if (!isalnum((unsigned char)*h))
            return false;
    return true;
}

typedef struct {
    kr_daemon *d;
    char dev_id[64], id[ID_MAX];
} icon_ctx;

static void icon_done(void *ud, bool ok, const char *path)
{
    icon_ctx *c = ud;
    kr_device *dev = kr_daemon_find_device(c->d, c->dev_id);
    state *st = dev ? kr_device_state(dev, &kr_plugin_notifications) : NULL;
    entry *e = st ? find_entry(st, c->id) : NULL;
    if (e) {
        e->icon_pending = false;
        if (ok)
            snprintf(e->icon, sizeof e->icon, "%s", path);
        present(e);
    }
    free(c);
}

/* ---- the packets -------------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    return strdup(s ? s : "");
}

/* "sender: text" lines of a conversation, the sender named when it changes. */
static char *conversation_text(const cJSON *conv, const char *first_sender)
{
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    out[0] = '\0';
    const char *prev = first_sender;
    const cJSON *m;
    cJSON_ArrayForEach(m, conv)
    {
        const cJSON *sender = cJSON_GetObjectItemCaseSensitive(m, "sender");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(m, "content");
        const char *s = cJSON_IsString(sender) ? sender->valuestring : "";
        const char *c = cJSON_IsString(content) ? content->valuestring : "";
        size_t need = len + strlen(s) + strlen(c) + 8;
        if (need >= cap) {
            while (need >= cap)
                cap *= 2;
            char *no = realloc(out, cap);
            if (!no) {
                free(out);
                return NULL;
            }
            out = no;
        }
        if (strcmp(s, prev) != 0) {
            len += (size_t)snprintf(out + len, cap - len, "%s:\n", s);
            prev = s;
        }
        len += (size_t)snprintf(out + len, cap - len, "%s\n", c);
    }
    if (len && out[len - 1] == '\n')
        out[len - 1] = '\0';
    return out;
}

static void on_cancel(state *st, const char *id)
{
    if (strncmp(id, ID_PREFIX, strlen(ID_PREFIX)) == 0) /* kdeconnect-android's prefix */
        id += strlen(ID_PREFIX);
    entry *e = find_entry(st, id);
    if (e)
        drop_entry(st, e, true);
}

static void on_notification(kr_daemon *d, kr_device *dev, state *st, const kr_packet *p)
{
    const char *id = kr_packet_str(p, "id");
    if (!id || !id[0] || strlen(id) >= ID_MAX)
        return;
    entry *e = find_entry(st, id);
    if (!e) {
        if (st->n == MAX_ENTRIES)
            drop_entry(st, st->v[0], true); /* the oldest */
        e = calloc(1, sizeof *e);
        if (!e)
            return;
        e->st = st;
        snprintf(e->id, sizeof e->id, "%s", id);
        st->v[st->n++] = e;
    }
    free_entry_text(e);

    kr_notify *n = kr_daemon_notify(d);
    const char *app = kr_packet_str(p, "appName"), *title = kr_packet_str(p, "title"),
               *text = kr_packet_str(p, "text"), *ticker = kr_packet_str(p, "ticker");
    app = app ? app : "";
    title = title ? title : "";
    text = text ? text : "";
    ticker = ticker ? ticker : "";
    const cJSON *conv = cJSON_GetObjectItemCaseSensitive(p->body, "conversation");
    char *chat = cJSON_IsArray(conv) && cJSON_GetArraySize(conv) ? conversation_text(conv, title) : NULL;
    if (chat)
        text = chat;

    /* The summary is the app; the body is "title: text", or what exists. */
    char et[512], ex[4096], eticker[512], body[5000];
    kr_notify_escape(n, title, et, sizeof et);
    kr_notify_escape(n, text, ex, sizeof ex);
    kr_notify_escape(n, ticker, eticker, sizeof eticker);
    if (!title[0] && !text[0])
        snprintf(body, sizeof body, "%s", eticker);
    else if (strcmp(app, title) == 0 || !title[0])
        snprintf(body, sizeof body, "%s", ex);
    else if (!text[0])
        snprintf(body, sizeof body, "%s", et);
    else if (chat)
        snprintf(body, sizeof body, "%s\n%s", et, ex);
    else
        snprintf(body, sizeof body, "%s: %s", et, ex);
    free(chat);
    e->summary = dup_str(app[0] ? app : kr_device_name(dev));
    e->body = dup_str(body);
    e->clearable = kr_packet_bool(p, "isClearable", false);
    e->silent = kr_packet_bool(p, "silent", false);

    const cJSON *acts = cJSON_GetObjectItemCaseSensitive(p->body, "actions"), *a;
    cJSON_ArrayForEach(a, acts)
    {
        if (cJSON_IsString(a) && e->n_actions < MAX_ACTIONS)
            e->actions[e->n_actions++] = strdup(a->valuestring);
    }

    /* The icon, from the cache or the phone. */
    e->icon[0] = '\0';
    const char *hash = kr_packet_str(p, "payloadHash");
    if (hash && hash_ok(hash) && !e->silent) {
        char path[KR_PATH_MAX];
        char rel[128];
        snprintf(rel, sizeof rel, "icons/%s", hash);
        if (kr_cache_path(path, sizeof path, rel)) {
            if (kr_file_exists(path)) {
                snprintf(e->icon, sizeof e->icon, "%s", path);
            } else if (!e->icon_pending && p->payload_port && p->payload_size > 0 &&
                       p->payload_size <= (int64_t)MAX_ICON) {
                icon_ctx *c = calloc(1, sizeof *c);
                if (c) {
                    c->d = d;
                    snprintf(c->dev_id, sizeof c->dev_id, "%s", kr_device_id(dev));
                    snprintf(c->id, sizeof c->id, "%s", e->id);
                    kr_make_parent_dirs(path);
                    char dir[KR_PATH_MAX];
                    if (kr_cache_path(dir, sizeof dir, "icons"))
                        chmod(dir, 0700);
                    e->icon_pending = kr_daemon_download_cb(d, dev, p, path, 0, icon_done, c);
                    if (!e->icon_pending)
                        free(c);
                }
            }
        }
    }
    present(e);
}

static void on_connect(kr_daemon *d, kr_device *dev)
{
    state *st = kr_device_state(dev, &kr_plugin_notifications);
    st->d = d;
    snprintf(st->dev_id, sizeof st->dev_id, "%s", kr_device_id(dev));
    kr_packet *p = kr_packet_new(REQUEST);
    if (p) {
        cJSON_AddBoolToObject(p->body, "request", true);
        kr_daemon_send(d, dev, p);
        kr_packet_free(p);
    }
}

static void on_disconnect(kr_daemon *d, kr_device *dev)
{
    (void)d;
    state *st = kr_device_state(dev, &kr_plugin_notifications);
    /* Their notifications go with the connection, as in the upstream. */
    while (st->n)
        drop_entry(st, st->v[0], true);
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    state *st = kr_device_state(dev, &kr_plugin_notifications);
    if (kr_packet_bool(p, "request", false))
        return; /* "Unexpected notification request" upstream; we have none to give */
    if (kr_packet_bool(p, "isCancel", false)) {
        const char *id = kr_packet_str(p, "id");
        if (id)
            on_cancel(st, id);
        return;
    }
    on_notification(d, dev, st, p);
}

const kr_plugin kr_plugin_notifications = {
    .name = "notifications",
    .incoming = incoming,
    .outgoing = outgoing,
    .state_size = sizeof(state),
    .on_connect = on_connect,
    .on_disconnect = on_disconnect,
    .on_packet = on_packet,
};
