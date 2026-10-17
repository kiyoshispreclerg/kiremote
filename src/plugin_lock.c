/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Locking the desktop from a phone, and telling it whether it is locked
 * (plugins/lockdevice upstream). Note: the Android app of the upstream
 * revision in upstream/ has no counterpart for this; it is for the other
 * KDE Connect clients (GSConnect, the iOS app, another desktop).
 *
 *   kdeconnect.lock          {"isLocked": bool}        our state, or theirs
 *                            {"lockResult": bool}      did their request work
 *   kdeconnect.lock.request  {"requestLocked": null}   tell me your state
 *                            {"setLocked": bool}       lock or unlock
 *
 * The state comes from logind's LockedHint of our session (system bus);
 * KiDesktop's locker, `xss-lock -- kilock`, sets it. Locking runs
 *
 *   [lock]
 *   command = loginctl lock-session         # what "lock" does
 *   allow_unlock = false                    # unlocking from a phone: off
 *   unlock_command = loginctl unlock-session
 *
 * The upstream unlocks whenever asked; here that must be switched on. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"

#define LOCK "kdeconnect.lock"
#define LOCK_REQUEST "kdeconnect.lock.request"
#define LOGIND "org.freedesktop.login1"
#define SESSION_IFACE "org.freedesktop.login1.Session"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"

static const char *const types[] = {LOCK, LOCK_REQUEST, NULL};

typedef struct {
    bool known, locked; /* the other side's state */
} peer_lock;

static struct {
    bool local_locked;
    char session[128]; /* our logind session's object path */
    bool listening;
} lk;

/* ---- our own state, from logind ------------------------------------------------ */

static void broadcast_state(kr_daemon *d)
{
    for (int i = 0; i < kr_daemon_device_count(d); i++) {
        kr_packet *p = kr_packet_new(LOCK);
        if (p) {
            cJSON_AddBoolToObject(p->body, "isLocked", lk.local_locked);
            kr_daemon_send(d, kr_daemon_device_at(d, i), p);
            kr_packet_free(p);
        }
    }
}

static void set_local(kr_daemon *d, bool locked)
{
    if (lk.local_locked == locked)
        return;
    lk.local_locked = locked;
    broadcast_state(d);
}

static void on_locked_hint(void *ud, DBusMessage *reply)
{
    DBusMessageIter it, var;
    dbus_bool_t b;
    if (reply && dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &var);
        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN) {
            dbus_message_iter_get_basic(&var, &b);
            set_local(ud, b);
        }
    }
}

static void on_signal(void *ud, DBusMessage *msg)
{
    if (!dbus_message_is_signal(msg, PROPS_IFACE, "PropertiesChanged") || !lk.session[0] ||
        !dbus_message_has_path(msg, lk.session))
        return;
    DBusMessageIter it, arr, e, kv, var;
    const char *iface;
    if (!dbus_message_iter_init(msg, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
        return;
    dbus_message_iter_get_basic(&it, &iface);
    if (strcmp(iface, SESSION_IFACE) != 0 || !dbus_message_iter_next(&it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
        return;
    dbus_message_iter_recurse(&it, &arr);
    for (e = arr; dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&e)) {
        const char *key;
        dbus_message_iter_recurse(&e, &kv);
        dbus_message_iter_get_basic(&kv, &key);
        if (strcmp(key, "LockedHint") != 0)
            continue;
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &var);
        dbus_bool_t b;
        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN) {
            dbus_message_iter_get_basic(&var, &b);
            set_local(ud, b);
        }
    }
}

static void on_session(void *ud, DBusMessage *reply)
{
    kr_daemon *d = ud;
    const char *path = NULL;
    if (!reply || !dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID) || !path)
        return;
    snprintf(lk.session, sizeof lk.session, "%s", path);
    kr_bus *bus = kr_daemon_system_bus(d);
    kr_bus_listen(bus, "type='signal',sender='" LOGIND "',interface='" PROPS_IFACE "',member='PropertiesChanged'",
                  on_signal, d);
    const char *iface = SESSION_IFACE, *prop = "LockedHint";
    kr_bus_call_args(bus, LOGIND, lk.session, PROPS_IFACE, "Get", on_locked_hint, d, 3000, DBUS_TYPE_STRING, &iface,
                     DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
}

static void init(kr_daemon *d)
{
    kr_bus *bus = kr_daemon_system_bus(d);
    if (!bus)
        return;
    dbus_uint32_t pid = (dbus_uint32_t)getpid();
    lk.listening = kr_bus_call_args(bus, LOGIND, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                    "GetSessionByPID", on_session, d, 3000, DBUS_TYPE_UINT32, &pid,
                                    DBUS_TYPE_INVALID);
}

static void shutdown_plugin(kr_daemon *d)
{
    kr_bus *bus = kr_daemon_system_bus(d);
    if (bus && lk.listening)
        kr_bus_unlisten(bus, on_signal, d);
    memset(&lk, 0, sizeof lk);
}

/* ---- doing it ----------------------------------------------------------------------- */

typedef struct {
    kr_daemon *d;
    char dev_id[64];
    bool lock;
} job;

static void on_command_done(void *ud, int status, const char *out, size_t len)
{
    (void)out;
    (void)len;
    job *j = ud;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    kr_device *dev = kr_daemon_find_device(j->d, j->dev_id);
    if (dev && j->lock) {
        kr_packet *p = kr_packet_new(LOCK);
        if (p) {
            cJSON_AddBoolToObject(p->body, "lockResult", ok);
            kr_daemon_send(j->d, dev, p);
            kr_packet_free(p);
        }
    }
    if (!ok)
        kr_log_warn(_("the %s command failed"), j->lock ? _("lock") : _("unlock"));
    /* Without logind to tell us, the command's word is all there is. */
    if (!lk.session[0] && ok)
        set_local(j->d, j->lock);
    free(j);
}

static void run_command(kr_daemon *d, kr_device *dev, bool lock)
{
    const kr_config *cfg = kr_daemon_config(d);
    const char *command;
    if (lock) {
        command = kr_config_get(cfg, "lock", "command");
        if (!command || !*command)
            command = "loginctl lock-session";
    } else {
        if (!kr_config_get_bool(cfg, "lock", "allow_unlock", false)) {
            kr_log_info(_("%s asked to unlock this computer; refused (set allow_unlock in [lock] to allow it)"),
                        kr_device_name(dev));
            return;
        }
        command = kr_config_get(cfg, "lock", "unlock_command");
        if (!command || !*command)
            command = "loginctl unlock-session";
    }
    job *j = calloc(1, sizeof *j);
    if (!j)
        return;
    j->d = d;
    j->lock = lock;
    snprintf(j->dev_id, sizeof j->dev_id, "%s", kr_device_id(dev));
    const char *argv[] = {"sh", "-c", command, NULL};
    if (!kr_proc_spawn_ex(kr_daemon_procs(d), argv, KR_PROC_USER, &(kr_proc_cb){.on_exit = on_command_done}, j))
        free(j);
}

/* ---- the plugin ------------------------------------------------------------------------ */

static void send_state(kr_daemon *d, kr_device *dev)
{
    kr_packet *p = kr_packet_new(LOCK);
    if (!p)
        return;
    cJSON_AddBoolToObject(p->body, "isLocked", lk.local_locked);
    kr_daemon_send(d, dev, p);
    kr_packet_free(p);
}

static void on_connect(kr_daemon *d, kr_device *dev)
{
    kr_packet *p = kr_packet_new(LOCK_REQUEST);
    if (!p)
        return;
    cJSON_AddNullToObject(p->body, "requestLocked");
    kr_daemon_send(d, dev, p);
    kr_packet_free(p);
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    peer_lock *pl = kr_device_state(dev, &kr_plugin_lock);
    if (cJSON_GetObjectItemCaseSensitive(p->body, "isLocked")) {
        pl->locked = kr_packet_bool(p, "isLocked", false);
        pl->known = true;
    }
    if (cJSON_GetObjectItemCaseSensitive(p->body, "requestLocked"))
        send_state(d, dev);
    if (cJSON_GetObjectItemCaseSensitive(p->body, "lockResult")) {
        bool ok = kr_packet_bool(p, "lockResult", false);
        kr_note *n = kr_notify_show(kr_daemon_notify(d),
                                    &(kr_note_spec){.summary = kr_device_name(dev),
                                                    .body = ok ? _("Remote lock successful") : _("Remote lock failed"),
                                                    .icon = ok ? "lock" : "error", .timeout_ms = -1},
                                    NULL, NULL);
        if (n)
            kr_note_detach(n);
    }
    if (cJSON_GetObjectItemCaseSensitive(p->body, "setLocked"))
        run_command(d, dev, kr_packet_bool(p, "setLocked", false));
}

static void describe(kr_daemon *d, const kr_device *dev, cJSON *out)
{
    (void)d;
    peer_lock *pl = kr_device_state((kr_device *)dev, &kr_plugin_lock);
    if (pl && pl->known)
        cJSON_AddBoolToObject(out, "locked", pl->locked);
}

const kr_plugin kr_plugin_lock = {
    .name = "lockdevice",
    .incoming = types,
    .outgoing = types,
    .state_size = sizeof(peer_lock),
    .init = init,
    .shutdown = shutdown_plugin,
    .on_connect = on_connect,
    .on_packet = on_packet,
    .describe = describe,
};
