/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Keep the screen from blanking or locking while a paired phone is
 * connected (plugins/screensaver-inhibit upstream). No packets: it works on
 * the connection itself, so it is off unless [plugins] screensaver_inhibit
 * says true, as in the upstream.
 *
 * The upstream asks org.freedesktop.ScreenSaver.Inhibit and gives the
 * cookie back when the last phone goes away. That interface is what GNOME,
 * KDE, Xfce and others serve; when nobody on the session bus does, this
 * falls back to `xdg-screensaver suspend <window>` tied to our hidden X
 * window, which xdg-screensaver undoes by itself if we die. */
#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "x11.h"

#define SS_NAME "org.freedesktop.ScreenSaver"
#define SS_IFACE "org.freedesktop.ScreenSaver"

/* The specification's object path, and the older one the upstream uses (the
 * services differ in which they serve; many serve both). */
static const char *const paths[] = {"/org/freedesktop/ScreenSaver", "/ScreenSaver"};

static struct {
    int users;               /* paired phones connected */
    int path;                /* index in paths of the one being used */
    enum { NONE, DBUS, XDG } method;
    dbus_uint32_t cookie;
    bool busy;               /* a call is in flight */
    char window[32];         /* for xdg-screensaver */
} ss;

static void reconcile(kr_daemon *d);

static void release(kr_daemon *d)
{
    if (ss.method == DBUS) {
        kr_bus *bus = kr_daemon_bus(d);
        kr_bus_call_args(bus, SS_NAME, paths[ss.path], SS_IFACE, "UnInhibit", NULL, NULL, 0, DBUS_TYPE_UINT32, &ss.cookie,
                         DBUS_TYPE_INVALID);
        /* The upstream pokes the screensaver afterwards: something that
         * manages it may not restart its idle timer by itself. */
        kr_bus_call_args(bus, SS_NAME, paths[ss.path], SS_IFACE, "SimulateUserActivity", NULL, NULL, 0, DBUS_TYPE_INVALID);
    } else if (ss.method == XDG) {
        const char *argv[] = {"xdg-screensaver", "resume", ss.window, NULL};
        kr_proc_spawn(kr_daemon_procs(d), argv, false, NULL, NULL);
    }
    ss.method = NONE;
    ss.cookie = 0;
}

static void use_xdg(kr_daemon *d)
{
    kr_x11 *x = kr_daemon_x11(d);
    if (!x || !kr_which("xdg-screensaver")) {
        kr_log_warn(_("cannot keep the screen awake: no screensaver service and no xdg-screensaver"));
        return;
    }
    snprintf(ss.window, sizeof ss.window, "0x%lx", (unsigned long)kr_x11_window(x));
    const char *argv[] = {"xdg-screensaver", "suspend", ss.window, NULL};
    if (kr_proc_spawn(kr_daemon_procs(d), argv, false, NULL, NULL))
        ss.method = XDG;
}

static void inhibit(kr_daemon *d);

static void on_inhibit(void *ud, DBusMessage *reply)
{
    kr_daemon *d = ud;
    dbus_uint32_t cookie = 0;
    if (reply && dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID)) {
        ss.busy = false;
        ss.method = DBUS;
        ss.cookie = cookie;
    } else if (ss.path + 1 < (int)(sizeof paths / sizeof *paths)) {
        ss.path++; /* the other object path */
        inhibit(d);
        return;
    } else {
        ss.busy = false;
        ss.path = 0;
        use_xdg(d);
    }
    reconcile(d);
}

/* Asks to inhibit on the object path being tried. */
static void inhibit(kr_daemon *d)
{
    const char *app = "Kiremote", *why = _("Phone is connected");
    if (!kr_bus_call_args(kr_daemon_bus(d), SS_NAME, paths[ss.path], SS_IFACE, "Inhibit", on_inhibit, d, 3000,
                          DBUS_TYPE_STRING, &app, DBUS_TYPE_STRING, &why, DBUS_TYPE_INVALID)) {
        ss.busy = false;
        ss.path = 0;
        use_xdg(d);
        reconcile(d);
    }
}

static void on_has_owner(void *ud, DBusMessage *reply)
{
    kr_daemon *d = ud;
    dbus_bool_t owned = FALSE;
    if (reply)
        dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &owned, DBUS_TYPE_INVALID);
    if (!owned) {
        ss.busy = false;
        use_xdg(d);
        reconcile(d);
        return;
    }
    ss.path = 0;
    inhibit(d);
}

/* Make the inhibition match whether a phone is connected. */
static void reconcile(kr_daemon *d)
{
    if (ss.busy)
        return;
    if (ss.users > 0 && ss.method == NONE) {
        kr_bus *bus = kr_daemon_bus(d);
        if (!kr_bus_alive(bus)) {
            use_xdg(d);
            return;
        }
        const char *name = SS_NAME;
        if (kr_bus_call_args(bus, DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "NameHasOwner",
                             on_has_owner, d, 3000, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID))
            ss.busy = true;
        else
            use_xdg(d);
    } else if (ss.users == 0 && ss.method != NONE) {
        release(d);
    }
}

static void on_connect(kr_daemon *d, kr_device *dev)
{
    (void)dev;
    ss.users++;
    reconcile(d);
}

static void on_disconnect(kr_daemon *d, kr_device *dev)
{
    (void)dev;
    if (ss.users > 0)
        ss.users--;
    reconcile(d);
}

static void shutdown_plugin(kr_daemon *d)
{
    ss.users = 0;
    if (ss.method != NONE)
        release(d);
    memset(&ss, 0, sizeof ss);
}

const kr_plugin kr_plugin_screensaver = {
    .name = "screensaver_inhibit",
    .off_by_default = true,
    .shutdown = shutdown_plugin,
    .on_connect = on_connect,
    .on_disconnect = on_disconnect,
};
