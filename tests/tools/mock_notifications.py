#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in org.freedesktop.Notifications server for tests/net.

  mock_notifications.py LOG COMMANDS [--no-actions]

Every call is appended to LOG ("NOTIFY id | summary | body | actions | timeout",
"CLOSE id"). Lines appended to COMMANDS act on it: "action ID KEY" sends
ActionInvoked, "dismiss ID" sends NotificationClosed (reason 2)."""
import sys, os
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib

IFACE = 'org.freedesktop.Notifications'
log = open(sys.argv[1], 'a', buffering=1)
cmd_path = sys.argv[2]
actions_ok = '--no-actions' not in sys.argv


class Server(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, '/org/freedesktop/Notifications')
        self.next_id = 0

    @dbus.service.method(IFACE, out_signature='as')
    def GetCapabilities(self):
        return ['body'] + (['actions'] if actions_ok else [])

    @dbus.service.method(IFACE, in_signature='susssasa{sv}i', out_signature='u')
    def Notify(self, app, replaces, icon, summary, body, actions, hints, timeout):
        self.next_id += 1
        acts = ','.join(str(a) for a in actions)
        urgent = int(hints.get('urgency', 1))
        log.write('NOTIFY %d | %s | %s | %s | timeout=%d | urgency=%d\n'
                  % (self.next_id, summary, body, acts, timeout, urgent))
        return dbus.UInt32(self.next_id)

    @dbus.service.method(IFACE, in_signature='u')
    def CloseNotification(self, nid):
        log.write('CLOSE %d\n' % nid)
        self.NotificationClosed(nid, 3)

    @dbus.service.signal(IFACE, signature='uu')
    def NotificationClosed(self, nid, reason):
        pass

    @dbus.service.signal(IFACE, signature='us')
    def ActionInvoked(self, nid, key):
        pass


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
name = dbus.service.BusName(IFACE, bus)
srv = Server(bus)
pos = 0


def poll():
    global pos
    try:
        with open(cmd_path) as f:
            f.seek(pos)
            for line in f.read().splitlines():
                parts = line.split()
                if parts[:1] == ['action']:
                    srv.ActionInvoked(dbus.UInt32(int(parts[1])), parts[2])
                elif parts[:1] == ['dismiss']:
                    srv.NotificationClosed(dbus.UInt32(int(parts[1])), dbus.UInt32(2))
            pos = f.tell()
    except FileNotFoundError:
        pass
    return True


GLib.timeout_add(100, poll)
log.write('READY\n')
GLib.MainLoop().run()
