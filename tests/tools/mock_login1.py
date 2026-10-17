#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in org.freedesktop.login1 on the (test) system bus.

  mock_login1.py COMMANDS

"locked true|false" lines appended to COMMANDS change LockedHint and emit
PropertiesChanged. Other sessions' signals are also faked ("other true") to
check the daemon ignores them."""
import sys
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib

NAME = 'org.freedesktop.login1'
SESSION = '/org/freedesktop/login1/session/_32'
OTHER = '/org/freedesktop/login1/session/_33'
SIFACE = 'org.freedesktop.login1.Session'
PROPS = 'org.freedesktop.DBus.Properties'
cmd_path = sys.argv[1]
state = {'LockedHint': dbus.Boolean(False)}


class Manager(dbus.service.Object):
    @dbus.service.method(NAME + '.Manager', in_signature='u', out_signature='o')
    def GetSessionByPID(self, pid):
        return dbus.ObjectPath(SESSION)


class Session(dbus.service.Object):
    @dbus.service.method(PROPS, in_signature='ss', out_signature='v')
    def Get(self, iface, key):
        return state[key]

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        return dbus.Dictionary(state, signature='sv')

    @dbus.service.signal(PROPS, signature='sa{sv}as')
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
busname = dbus.service.BusName(NAME, bus)
Manager(bus, '/org/freedesktop/login1')
session = Session(bus, SESSION)
other = Session(bus, OTHER)
pos = 0


def poll():
    global pos
    try:
        with open(cmd_path) as f:
            f.seek(pos)
            for line in f.read().splitlines():
                parts = line.split()
                if parts[:1] == ['locked']:
                    state['LockedHint'] = dbus.Boolean(parts[1] == 'true')
                    session.PropertiesChanged(SIFACE, {'LockedHint': state['LockedHint']}, [])
                elif parts[:1] == ['other']:
                    other.PropertiesChanged(SIFACE, {'LockedHint': dbus.Boolean(parts[1] == 'true')}, [])
            pos = f.tell()
    except FileNotFoundError:
        pass
    return True


GLib.timeout_add(100, poll)
open(cmd_path + '.ready', 'w').write('1')
GLib.MainLoop().run()
