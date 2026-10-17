#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in org.freedesktop.ScreenSaver for tests/net: mock_screensaver.py LOG
Logs "INHIBIT app reason cookie", "UNINHIBIT cookie" and "ACTIVITY"."""
import sys
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib

IFACE = 'org.freedesktop.ScreenSaver'
log = open(sys.argv[1], 'a', buffering=1)


class S(dbus.service.Object):
    cookie = 41

    @dbus.service.method(IFACE, in_signature='ss', out_signature='u')
    def Inhibit(self, app, reason):
        S.cookie += 1
        log.write('INHIBIT %s | %s | %d\n' % (app, reason, S.cookie))
        return dbus.UInt32(S.cookie)

    @dbus.service.method(IFACE, in_signature='u')
    def UnInhibit(self, cookie):
        log.write('UNINHIBIT %d\n' % cookie)

    @dbus.service.method(IFACE)
    def SimulateUserActivity(self):
        log.write('ACTIVITY\n')


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
name = dbus.service.BusName(IFACE, bus)
service = S(bus, '/ScreenSaver')  # (only the old path: tests the fallback) kept: dbus-python unexports a collected object
log.write('READY\n')
GLib.MainLoop().run()
