# Kiremote

[Português](README.md)

Connects your computer to your phone, compatible with **KDE Connect**: use
the official KDE Connect app on Android (or iOS) and Kiremote on the
desktop side. Made for KiDesktop, but runs on any X11 desktop.

Same idea as the other KiDesktop programs: light and fast. The daemon is
plain C with OpenSSL and Xlib, no Qt or KDE Frameworks; the interface is
GTK2 and only stays open while you use it.

It will also do what KDE Connect doesn't: **mirror and control the Android
screen** (through `scrcpy-server`, with its own client, no SDL).

## Status

In development: the protocol core (phase 1) is done and phase 2 (plugins)
is half way. `kiremoted` discovers devices on the network, pairs, and has:
ping, files, battery, cellular signal, find my phone and find this
computer, call notifications (and muting), pausing music during a call,
the phone controlling the desktop's media players, system volume, a shared
clipboard, text and links, the phone's notifications on the desktop, remote
commands, the phone as mouse and keyboard, a presentation pointer, locking
and (optionally) keeping the screen awake. `kiremote-cli` drives it from the
terminal. There is no interface or tray
icon yet (phase 3). See [PLANO.md](PLANO.md) (in Portuguese) for what is
planned and in which order.

## Usage

```sh
build/kiremoted -v &            # the daemon (messages go to the terminal)
build/kiremote-cli -l           # paired devices and the ones around
build/kiremote-cli -d Phone --pair        # ask to pair and wait
build/kiremote-cli -d Phone --ping
build/kiremote-cli -d Phone --share photo.jpg
build/kiremote-cli -d Phone --find-phone     # the phone rings
build/kiremote-cli -d Phone --mute-call      # silence the ringing call
build/kiremote-cli --stop-ring               # stop the sound when the phone looks for this computer
build/kiremote-cli -d Phone --share-text "hi"     # goes to the phone's clipboard
build/kiremote-cli -d Phone --share-url https://example.org
```

Notifications, media control and pause-on-call need a D-Bus session
(`$DBUS_SESSION_BUS_ADDRESS`); without one the daemon still works but only
writes to its log. Volume uses `pactl`, and the "find this computer" sound
uses `paplay`, `pw-play` or `aplay`. Each plugin can be turned off in the
`[plugins]` section of the .ini. The clipboard, mouse/keyboard and
presentation pointer need X11 (`$DISPLAY`); the commands the phone may run
are only the ones in the `[runcommand]` section.

On the phone, open the KDE Connect app: the computer shows up in the list.
Either side can start pairing; both show the same verification key. When
the phone asks, the daemon logs the key and you accept with
`kiremote-cli -d <device> --pair` (or refuse with `--unpair`). Received
files go to your downloads folder; the settings are described in
[data/kiremote.ini](data/kiremote.ini).

If the computer doesn't show up on the phone, check that the firewall lets
UDP and TCP 1716 to 1764 through, or add the computer's IP in the app ("Add
devices by IP"). Kiremote can run next to KDE Connect on the same machine:
the phone sees them as two different computers.

## Building

Needs a C11 compiler, `make`, `pkg-config`, OpenSSL and gettext (`msgfmt`).

```sh
make
make test        # unit tests, fuzz corpus, and two daemons in network namespaces
build/kiremoted --help
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

Not affiliated with KDE. "KDE Connect" is only used to say which protocol
Kiremote is compatible with.
