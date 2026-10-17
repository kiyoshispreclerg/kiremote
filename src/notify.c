/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#define NOTIF_DEST "org.freedesktop.Notifications"
#define NOTIF_PATH "/org/freedesktop/Notifications"
#define NOTIF_IFACE "org.freedesktop.Notifications"
#define MAX_NOTES 64

struct kr_note {
    kr_notify *n;
    dbus_uint32_t id; /* 0 until the server answers */
    bool want_close, detached;
    kr_note_cb cb;
    void *ud;
};

struct kr_notify {
    kr_bus *bus;
    bool actions, markup;
    kr_note *notes[MAX_NOTES];
    int n_notes;
};

static void forget(kr_notify *n, kr_note *note)
{
    for (int i = 0; i < n->n_notes; i++)
        if (n->notes[i] == note) {
            n->notes[i] = n->notes[--n->n_notes];
            break;
        }
    free(note);
}

static kr_note *by_id(kr_notify *n, dbus_uint32_t id)
{
    for (int i = 0; i < n->n_notes; i++)
        if (n->notes[i]->id == id && id)
            return n->notes[i];
    return NULL;
}

static void send_close(kr_notify *n, dbus_uint32_t id)
{
    DBusMessage *m = kr_bus_new_call(NOTIF_DEST, NOTIF_PATH, NOTIF_IFACE, "CloseNotification");
    if (m && dbus_message_append_args(m, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID))
        kr_bus_call(n->bus, m, NULL, NULL, 0);
    else if (m)
        dbus_message_unref(m);
}

static void on_signal(void *ud, DBusMessage *msg)
{
    kr_notify *n = ud;
    if (!dbus_message_has_interface(msg, NOTIF_IFACE))
        return;
    dbus_uint32_t id = 0;
    if (dbus_message_has_member(msg, "ActionInvoked")) {
        const char *key = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &key,
                                  DBUS_TYPE_INVALID)) {
            kr_note *note = by_id(n, id);
            if (note && !note->detached && note->cb.on_action)
                note->cb.on_action(note->ud, key);
        }
    } else if (dbus_message_has_member(msg, "NotificationClosed")) {
        dbus_uint32_t reason = KR_NOTE_UNKNOWN;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID) ||
            dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) {
            kr_note *note = by_id(n, id);
            if (!note)
                return;
            kr_note_cb cb = note->cb;
            void *cud = note->ud;
            bool detached = note->detached;
            forget(n, note);
            if (!detached && cb.on_closed)
                cb.on_closed(cud, (int)reason);
        }
    }
}

static void on_caps(void *ud, DBusMessage *reply)
{
    kr_notify *n = ud;
    DBusMessageIter it, arr;
    if (!reply || !dbus_message_iter_init(reply, &it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
        return;
    dbus_message_iter_recurse(&it, &arr);
    for (; dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING; dbus_message_iter_next(&arr)) {
        const char *cap;
        dbus_message_iter_get_basic(&arr, &cap);
        if (strcmp(cap, "actions") == 0)
            n->actions = true;
        else if (strcmp(cap, "body-markup") == 0)
            n->markup = true;
    }
    kr_log_debug("notification server %s actions", n->actions ? "supports" : "does not support");
}

kr_notify *kr_notify_new(kr_bus *bus)
{
    kr_notify *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->bus = bus;
    if (kr_bus_alive(bus)) {
        kr_bus_listen(bus, "type='signal',interface='" NOTIF_IFACE "',path='" NOTIF_PATH "'",
                      on_signal, n);
        kr_bus_call(bus, kr_bus_new_call(NOTIF_DEST, NOTIF_PATH, NOTIF_IFACE, "GetCapabilities"),
                    on_caps, n, 2000);
    }
    return n;
}

void kr_notify_free(kr_notify *n)
{
    if (!n)
        return;
    if (kr_bus_alive(n->bus))
        kr_bus_unlisten(n->bus, on_signal, n);
    /* The owners of notifications still open are told they are gone, so
     * what they kept for them (a button's context) is theirs to free. */
    while (n->n_notes) {
        kr_note *note = n->notes[--n->n_notes];
        kr_note_cb cb = note->cb;
        void *ud = note->ud;
        bool detached = note->detached;
        free(note);
        if (!detached && cb.on_closed)
            cb.on_closed(ud, KR_NOTE_UNKNOWN);
    }
    free(n);
}

bool kr_notify_has_actions(const kr_notify *n)
{
    return n && n->actions;
}

bool kr_notify_has_markup(const kr_notify *n)
{
    return n && n->markup;
}

void kr_notify_escape(const kr_notify *n, const char *in, char *out, size_t size)
{
    size_t o = 0;
    for (; *in && o + 1 < size; in++) {
        const char *rep = !n || !n->markup ? NULL : *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : NULL;
        if (!rep) {
            out[o++] = *in;
            continue;
        }
        size_t len = strlen(rep);
        if (o + len + 1 > size)
            break;
        memcpy(out + o, rep, len);
        o += len;
    }
    out[o] = '\0';
}

static void on_notify_reply(void *ud, DBusMessage *reply)
{
    kr_note *note = ud;
    dbus_uint32_t id = 0;
    if (!reply || !dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) {
        /* No server, or it refused: nothing to wait for. */
        kr_notify *n = note->n;
        kr_note_cb cb = note->cb;
        void *cud = note->ud;
        bool detached = note->detached;
        forget(n, note);
        if (!detached && cb.on_closed)
            cb.on_closed(cud, KR_NOTE_UNKNOWN);
        return;
    }
    note->id = id;
    if (note->want_close)
        send_close(note->n, id);
}

/* The Notify call for spec; replaces is the id to replace (0: a new one). */
static DBusMessage *build_notify(kr_notify *n, const kr_note_spec *spec, dbus_uint32_t replaces)
{
    DBusMessage *m = kr_bus_new_call(NOTIF_DEST, NOTIF_PATH, NOTIF_IFACE, "Notify");
    if (!m)
        return NULL;
    const char *app = "Kiremote", *icon = spec->icon ? spec->icon : "", *body = spec->body ? spec->body : "";
    const char *summary = spec->summary;
    dbus_int32_t timeout = spec->timeout_ms;
    DBusMessageIter it, arr, dict, entry, var;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replaces);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &summary);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &body);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    if (n->actions)
        for (const char *const *a = spec->actions; a && a[0] && a[1]; a += 2) {
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &a[0]);
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &a[1]);
        }
    dbus_message_iter_close_container(&it, &arr);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (spec->urgent) {
        const char *key = "urgency";
        unsigned char level = 2;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "y", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BYTE, &level);
        dbus_message_iter_close_container(&entry, &var);
        dbus_message_iter_close_container(&dict, &entry);
    }
    dbus_message_iter_close_container(&it, &dict);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeout);
    return m;
}

kr_note *kr_notify_show(kr_notify *n, const kr_note_spec *spec, const kr_note_cb *cb, void *ud)
{
    if (!n || !kr_bus_alive(n->bus) || n->n_notes == MAX_NOTES) {
        kr_log_info("%s%s%s", spec->summary, spec->body ? ": " : "", spec->body ? spec->body : "");
        return NULL;
    }
    DBusMessage *m = build_notify(n, spec, 0);
    kr_note *note = calloc(1, sizeof *note);
    if (!m || !note) {
        if (m)
            dbus_message_unref(m);
        free(note);
        return NULL;
    }
    note->n = n;
    if (cb)
        note->cb = *cb;
    note->ud = ud;
    n->notes[n->n_notes++] = note;
    if (!kr_bus_call(n->bus, m, on_notify_reply, note, 5000)) {
        forget(n, note);
        return NULL;
    }
    return note;
}

static void on_update_reply(void *ud, DBusMessage *reply)
{
    (void)ud;
    (void)reply; /* same id as before: nothing to learn */
}

bool kr_note_update(kr_note *note, const kr_note_spec *spec)
{
    if (!note->id || note->want_close)
        return false;
    DBusMessage *m = build_notify(note->n, spec, note->id);
    return m && kr_bus_call(note->n->bus, m, on_update_reply, NULL, 5000);
}

void kr_note_close(kr_note *note)
{
    if (!note->id)
        note->want_close = true;
    else
        send_close(note->n, note->id);
}

void kr_note_detach(kr_note *note)
{
    note->detached = true;
}
