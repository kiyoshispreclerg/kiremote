#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in MPRIS media player for tests/net.

  mock_player.py NAME LOG COMMANDS

Appears as org.mpris.MediaPlayer2.NAME. Calls are appended to LOG ("CALL
Pause", "SET Volume 0.5", "SEEK 5000000"). Lines appended to COMMANDS change
its state and emit PropertiesChanged: "status Playing", "title Foo",
"artist A", "volume 0.3"."""
import sys
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib

ROOT = 'org.mpris.MediaPlayer2'
PLAYER = 'org.mpris.MediaPlayer2.Player'
PROPS = 'org.freedesktop.DBus.Properties'
name, log_path, cmd_path = sys.argv[1:4]
log = open(log_path, 'a', buffering=1)

state = {
    'PlaybackStatus': dbus.String('Paused'),
    'Volume': dbus.Double(0.5),
    'Position': dbus.Int64(3000000),
    'LoopStatus': dbus.String('None'),
    'Shuffle': dbus.Boolean(False),
    'CanPause': dbus.Boolean(True), 'CanPlay': dbus.Boolean(True),
    'CanGoNext': dbus.Boolean(True), 'CanGoPrevious': dbus.Boolean(False),
    'CanSeek': dbus.Boolean(True),
}
meta = {'xesam:title': 'Song', 'xesam:artist': ['Band'], 'xesam:album': 'Album',
        'mpris:length': dbus.Int64(180000000), 'mpris:artUrl': 'file:///tmp/none.jpg'}


def metadata():
    d = dbus.Dictionary(signature='sv')
    for k, v in meta.items():
        d[k] = dbus.Array(v, signature='s') if isinstance(v, list) else v
    return d


class Player(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, '/org/mpris/MediaPlayer2')

    def all(self, iface):
        if iface == ROOT:
            return {'Identity': dbus.String('Test Player'), 'CanQuit': dbus.Boolean(False)}
        d = dict(state)
        d['Metadata'] = metadata()
        return d

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        return dbus.Dictionary(self.all(iface), signature='sv')

    @dbus.service.method(PROPS, in_signature='ss', out_signature='v')
    def Get(self, iface, key):
        return self.all(iface)[key]

    @dbus.service.method(PROPS, in_signature='ssv')
    def Set(self, iface, key, value):
        log.write('SET %s %s\n' % (key, value))
        state[key] = value
        self.PropertiesChanged(iface, {key: value}, [])

    @dbus.service.signal(PROPS, signature='sa{sv}as')
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    @dbus.service.signal(PLAYER, signature='x')
    def Seeked(self, pos):
        pass

    def call(self, what):
        log.write('CALL %s\n' % what)
        if what in ('Pause', 'Stop'):
            self.change('PlaybackStatus', dbus.String('Paused'))
        elif what == 'Play':
            self.change('PlaybackStatus', dbus.String('Playing'))

    def change(self, key, value):
        state[key] = value
        self.PropertiesChanged(PLAYER, {key: value}, [])

    @dbus.service.method(PLAYER)
    def Play(self): self.call('Play')

    @dbus.service.method(PLAYER)
    def Pause(self): self.call('Pause')

    @dbus.service.method(PLAYER)
    def PlayPause(self): self.call('PlayPause')

    @dbus.service.method(PLAYER)
    def Stop(self): self.call('Stop')

    @dbus.service.method(PLAYER)
    def Next(self): self.call('Next')

    @dbus.service.method(PLAYER)
    def Previous(self): self.call('Previous')

    @dbus.service.method(PLAYER, in_signature='x')
    def Seek(self, offset):
        log.write('SEEK %d\n' % offset)
        state['Position'] = dbus.Int64(int(state['Position']) + int(offset))
        self.Seeked(state['Position'])


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
busname = dbus.service.BusName('org.mpris.MediaPlayer2.' + name, bus)
player = Player(bus)
pos = 0


def poll():
    global pos
    try:
        with open(cmd_path) as f:
            f.seek(pos)
            for line in f.read().splitlines():
                parts = line.split(None, 1)
                if not parts:
                    continue
                key, val = parts[0], parts[1] if len(parts) > 1 else ''
                if key == 'status':
                    player.change('PlaybackStatus', dbus.String(val))
                elif key == 'volume':
                    player.change('Volume', dbus.Double(float(val)))
                elif key in ('title', 'artist', 'album'):
                    meta['xesam:' + key] = [val] if key == 'artist' else val
                    player.PropertiesChanged(PLAYER, {'Metadata': metadata()}, [])
            pos = f.tell()
    except FileNotFoundError:
        pass
    return True


GLib.timeout_add(100, poll)
log.write('READY\n')
GLib.MainLoop().run()
