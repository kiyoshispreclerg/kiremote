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
    bool actions;
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
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) {
            kr_note *note = by_id(n, id);
            if (!note)
                return;
            kr_note_cb cb = note->cb;
            void *cud = note->ud;
            bool detached = note->detached;
            forget(n, note);
            if (!detached && cb.on_closed)
                cb.on_closed(cud);
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
    for (int i = 0; i < n->n_notes; i++)
        free(n->notes[i]);
    free(n);
}

bool kr_notify_has_actions(const kr_notify *n)
{
    return n && n->actions;
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
            cb.on_closed(cud);
        return;
    }
    note->id = id;
    if (note->want_close)
        send_close(note->n, id);
}

kr_note *kr_notify_show(kr_notify *n, const kr_note_spec *spec, const kr_note_cb *cb, void *ud)
{
    if (!n || !kr_bus_alive(n->bus) || n->n_notes == MAX_NOTES) {
        kr_log_info("%s%s%s", spec->summary, spec->body ? ": " : "", spec->body ? spec->body : "");
        return NULL;
    }
    DBusMessage *m = kr_bus_new_call(NOTIF_DEST, NOTIF_PATH, NOTIF_IFACE, "Notify");
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

    const char *app = "Kiremote", *icon = spec->icon ? spec->icon : "", *body = spec->body ? spec->body : "";
    const char *summary = spec->summary;
    dbus_uint32_t replaces = 0;
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

    n->notes[n->n_notes++] = note;
    if (!kr_bus_call(n->bus, m, on_notify_reply, note, 5000)) {
        forget(n, note);
        return NULL;
    }
    return note;
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
