/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"
#include "util.h"

#define MAX_WATCHES 16
#define MAX_TIMEOUTS 16
#define MAX_LISTENERS 32

typedef struct {
    kr_bus_signal_fn fn;
    void *ud;
} listener;

typedef struct {
    DBusTimeout *t;
    double at; /* monotonic time it fires */
} timer;

struct kr_bus {
    DBusConnection *conn; /* NULL once gone */
    DBusWatch *watches[MAX_WATCHES];
    int n_watches;
    DBusWatch *polled[MAX_WATCHES]; /* what kr_bus_pollfds handed out */
    int n_polled;
    timer timers[MAX_TIMEOUTS];
    int n_timers;
    listener listeners[MAX_LISTENERS];
    int n_listeners;
    bool need_dispatch;
};

typedef struct {
    kr_bus_reply_fn fn;
    void *ud;
} call;

/* ---- watches and timeouts --------------------------------------------------- */

static dbus_bool_t add_watch(DBusWatch *w, void *data)
{
    kr_bus *b = data;
    if (b->n_watches == MAX_WATCHES)
        return FALSE;
    b->watches[b->n_watches++] = w;
    return TRUE;
}

static void remove_watch(DBusWatch *w, void *data)
{
    kr_bus *b = data;
    for (int i = 0; i < b->n_watches; i++)
        if (b->watches[i] == w) {
            b->watches[i] = b->watches[--b->n_watches];
            break;
        }
    for (int i = 0; i < b->n_polled; i++)
        if (b->polled[i] == w)
            b->polled[i] = NULL;
}

static void toggle_watch(DBusWatch *w, void *data)
{
    (void)w;
    (void)data; /* enabled state is read when the pollfds are built */
}

static dbus_bool_t add_timeout(DBusTimeout *t, void *data)
{
    kr_bus *b = data;
    if (b->n_timers == MAX_TIMEOUTS)
        return FALSE;
    b->timers[b->n_timers++] = (timer){.t = t, .at = kr_now() + dbus_timeout_get_interval(t) / 1000.0};
    return TRUE;
}

static void remove_timeout(DBusTimeout *t, void *data)
{
    kr_bus *b = data;
    for (int i = 0; i < b->n_timers; i++)
        if (b->timers[i].t == t) {
            b->timers[i] = b->timers[--b->n_timers];
            break;
        }
}

static void toggle_timeout(DBusTimeout *t, void *data)
{
    kr_bus *b = data;
    for (int i = 0; i < b->n_timers; i++)
        if (b->timers[i].t == t)
            b->timers[i].at = kr_now() + dbus_timeout_get_interval(t) / 1000.0;
}

static void dispatch_status(DBusConnection *c, DBusDispatchStatus st, void *data)
{
    (void)c;
    kr_bus *b = data;
    if (st == DBUS_DISPATCH_DATA_REMAINS)
        b->need_dispatch = true;
}

/* ---- signals ----------------------------------------------------------------- */

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *msg, void *data)
{
    (void)c;
    kr_bus *b = data;
    if (dbus_message_is_signal(msg, DBUS_INTERFACE_LOCAL, "Disconnected")) {
        kr_log_debug("the session bus went away");
        b->conn = NULL;
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL)
        for (int i = 0; i < b->n_listeners; i++)
            b->listeners[i].fn(b->listeners[i].ud, msg);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---- open / close -------------------------------------------------------------- */

static bool session_address(char *out, size_t size)
{
    const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (env && *env)
        return kr_pathf(out, size, "%s", env);
    const char *run = getenv("XDG_RUNTIME_DIR");
    char path[KR_PATH_MAX];
    struct stat st;
    if (run && run[0] == '/' && kr_pathf(path, sizeof path, "%s/bus", run) &&
        stat(path, &st) == 0 && S_ISSOCK(st.st_mode))
        return kr_pathf(out, size, "unix:path=%s", path);
    return false;
}

kr_bus *kr_bus_open(void)
{
    char addr[KR_PATH_MAX];
    if (!session_address(addr, sizeof addr)) {
        kr_log_debug("no session bus address");
        return NULL;
    }
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_connection_open_private(addr, &err);
    if (!c || !dbus_bus_register(c, &err)) {
        kr_log_debug("session bus: %s", dbus_error_is_set(&err) ? err.message : "?");
        dbus_error_free(&err);
        if (c) {
            dbus_connection_close(c);
            dbus_connection_unref(c);
        }
        return NULL;
    }
    kr_bus *b = calloc(1, sizeof *b);
    if (!b) {
        dbus_connection_close(c);
        dbus_connection_unref(c);
        return NULL;
    }
    b->conn = c;
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    if (!dbus_connection_set_watch_functions(c, add_watch, remove_watch, toggle_watch, b, NULL) ||
        !dbus_connection_set_timeout_functions(c, add_timeout, remove_timeout, toggle_timeout, b, NULL) ||
        !dbus_connection_add_filter(c, filter, b, NULL)) {
        kr_bus_free(b);
        return NULL;
    }
    dbus_connection_set_dispatch_status_function(c, dispatch_status, b, NULL);
    b->need_dispatch = true; /* whatever the handshake left */
    return b;
}

void kr_bus_free(kr_bus *b)
{
    if (!b)
        return;
    DBusConnection *c = b->conn;
    if (c) {
        dbus_connection_set_watch_functions(c, NULL, NULL, NULL, NULL, NULL);
        dbus_connection_set_timeout_functions(c, NULL, NULL, NULL, NULL, NULL);
        dbus_connection_set_dispatch_status_function(c, NULL, NULL, NULL);
        dbus_connection_close(c);
        dbus_connection_unref(c);
    }
    free(b);
}

bool kr_bus_alive(const kr_bus *b)
{
    return b && b->conn;
}

/* ---- calls ---------------------------------------------------------------------- */

DBusMessage *kr_bus_new_call(const char *dest, const char *path, const char *iface,
                             const char *member)
{
    return dbus_message_new_method_call(dest, path, iface, member);
}

static void call_done(DBusPendingCall *pc, void *data)
{
    call *c = data;
    DBusMessage *reply = dbus_pending_call_steal_reply(pc);
    if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        kr_log_debug("D-Bus error: %s", dbus_message_get_error_name(reply));
        dbus_message_unref(reply);
        reply = NULL;
    }
    if (c->fn)
        c->fn(c->ud, reply);
    if (reply)
        dbus_message_unref(reply);
}

bool kr_bus_call(kr_bus *b, DBusMessage *msg, kr_bus_reply_fn fn, void *ud, int timeout_ms)
{
    if (!msg)
        return false;
    if (!b || !b->conn) {
        dbus_message_unref(msg);
        return false;
    }
    bool ok = false;
    if (!fn) {
        dbus_message_set_no_reply(msg, TRUE);
        ok = dbus_connection_send(b->conn, msg, NULL);
    } else {
        call *c = malloc(sizeof *c);
        DBusPendingCall *pc = NULL;
        if (c) {
            *c = (call){.fn = fn, .ud = ud};
            ok = dbus_connection_send_with_reply(b->conn, msg, &pc, timeout_ms) && pc &&
                 dbus_pending_call_set_notify(pc, call_done, c, free);
        }
        if (!ok)
            free(c);
        if (pc) {
            if (!ok)
                dbus_pending_call_cancel(pc);
            dbus_pending_call_unref(pc);
        }
    }
    dbus_message_unref(msg);
    return ok;
}

bool kr_bus_listen(kr_bus *b, const char *rule, kr_bus_signal_fn fn, void *ud)
{
    if (!b || !b->conn || b->n_listeners == MAX_LISTENERS)
        return false;
    bool have = false;
    for (int i = 0; i < b->n_listeners; i++)
        have |= b->listeners[i].fn == fn && b->listeners[i].ud == ud;
    if (!have)
        b->listeners[b->n_listeners++] = (listener){fn, ud};
    DBusMessage *m = kr_bus_new_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "AddMatch");
    if (!m || !dbus_message_append_args(m, DBUS_TYPE_STRING, &rule, DBUS_TYPE_INVALID)) {
        if (m)
            dbus_message_unref(m);
        return false;
    }
    return kr_bus_call(b, m, NULL, NULL, 0);
}

void kr_bus_unlisten(kr_bus *b, kr_bus_signal_fn fn, void *ud)
{
    for (int i = 0; i < b->n_listeners;) {
        if (b->listeners[i].fn == fn && b->listeners[i].ud == ud)
            b->listeners[i] = b->listeners[--b->n_listeners];
        else
            i++;
    }
}

/* ---- the loop's side ------------------------------------------------------------- */

int kr_bus_pollfds(kr_bus *b, struct pollfd *fds, int max)
{
    int n = 0;
    b->n_polled = 0;
    for (int i = 0; b->conn && i < b->n_watches && n < max; i++) {
        DBusWatch *w = b->watches[i];
        if (!dbus_watch_get_enabled(w))
            continue;
        unsigned fl = dbus_watch_get_flags(w);
        short ev = (short)((fl & DBUS_WATCH_READABLE ? POLLIN : 0) | (fl & DBUS_WATCH_WRITABLE ? POLLOUT : 0));
        fds[n] = (struct pollfd){.fd = dbus_watch_get_unix_fd(w), .events = ev};
        b->polled[b->n_polled++] = w;
        n++;
    }
    return n;
}

void kr_bus_dispatch(kr_bus *b, const struct pollfd *fds, int n)
{
    if (!b->conn)
        return;
    for (int i = 0; i < n && i < b->n_polled; i++) {
        DBusWatch *w = b->polled[i];
        if (!w || !fds[i].revents)
            continue;
        unsigned fl = 0;
        if (fds[i].revents & POLLIN)
            fl |= DBUS_WATCH_READABLE;
        if (fds[i].revents & POLLOUT)
            fl |= DBUS_WATCH_WRITABLE;
        if (fds[i].revents & POLLERR)
            fl |= DBUS_WATCH_ERROR;
        if (fds[i].revents & POLLHUP)
            fl |= DBUS_WATCH_HANGUP;
        dbus_watch_handle(w, fl);
    }
    double now = kr_now();
    for (int i = 0; b->conn && i < b->n_timers; i++)
        if (b->timers[i].at <= now) {
            DBusTimeout *t = b->timers[i].t;
            b->timers[i].at = now + dbus_timeout_get_interval(t) / 1000.0;
            if (dbus_timeout_get_enabled(t))
                dbus_timeout_handle(t);
        }
    b->need_dispatch = false;
    while (b->conn && dbus_connection_dispatch(b->conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;
    if (b->conn && dbus_connection_get_dispatch_status(b->conn) == DBUS_DISPATCH_DATA_REMAINS)
        b->need_dispatch = true;
}

double kr_bus_deadline(const kr_bus *b)
{
    if (!b->conn)
        return 0;
    if (b->need_dispatch)
        return kr_now();
    double next = 0;
    for (int i = 0; i < b->n_timers; i++)
        if (dbus_timeout_get_enabled(b->timers[i].t) && (next == 0 || b->timers[i].at < next))
            next = b->timers[i].at;
    return next;
}
