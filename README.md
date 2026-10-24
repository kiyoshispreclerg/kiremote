# Kiremote

[Português](README.pt-BR.md)

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
commands, the phone as mouse and keyboard, browsing the phone's files, a presentation pointer, locking
and (optionally) keeping the screen awake. `kiremote-cli` drives it from the
terminal, and `kiremote` (GTK2) is the window: devices (battery, signal, ping,
find, send files and text, also by dropping files on them, browse, pair and
unpair), the pairing question with the verification key, and every setting,
built from what the daemon reports. Outside KiDesktop it puts an icon in the
tray (`[ui] tray` in the .ini). On KiDesktop the same interface is a kiconf tab, an xisserve page
(`xisserve --kiremote`) and an xispanel widget (`WIDGET ... kiremote`), which
also asks the pairing question with `xisserve --question`. See [PLANO.md](PLANO.md) (in Portuguese) for what is
planned and in which order.

## Usage

```sh
build/kiremote             # the window (needs GTK2 to build); --settings, --hidden
```

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
build/kiremote-cli --status                  # the daemon and the devices (--json for scripts)
build/kiremote-cli --schema                  # every setting, with its description
build/kiremote-cli --get clipboard.auto_share
build/kiremote-cli --set clipboard.auto_share=off   # written to the .ini, comments kept
build/kiremote-cli --watch                   # events (device changed, pairing request...) as JSON
```

Notifications, media control and pause-on-call need a D-Bus session
(`$DBUS_SESSION_BUS_ADDRESS`); without one the daemon still works but only
writes to its log. Volume uses `pactl`, and the "find this computer" sound
uses `paplay`, `pw-play` or `aplay`. Each plugin can be turned off in the
`[plugins]` section of the .ini. The clipboard, mouse/keyboard and
presentation pointer need X11 (`$DISPLAY`); the commands the phone may run
are only the ones in the `[runcommand]` section. To browse the phone's files
(`kiremote-cli -d PHONE --browse-files`) `sshfs` is the best way; without it
the daemon hands the `sftp://` address to `xdg-open`.

On the phone, open the KDE Connect app: the computer shows up in the list.
Either side can start pairing; both show the same verification key. When
the phone asks, the daemon logs the key and you accept with
`kiremote-cli -d <device> --pair` (or refuse with `--unpair`). Received
files go to your downloads folder; the settings are described in
[data/kiremote.ini](data/kiremote.ini).

Mirroring (phase 5: picture, sound, mouse, keyboard, clipboard and Wi-Fi): turn
on USB debugging on the phone, plug it in and run

```sh
scripts/fetch-scrcpy-server.sh   # once: downloads and checks the scrcpy-server (into build/)
build/kiremote-mirror            # -s SERIAL with more than one phone; -m 1024 limits the size
```

It uses the system's `adb` and the `scrcpy-server` of the version pinned in
`config.mk`; the phone does not need the scrcpy app. As in scrcpy, the left
button touches, the right one is Back, the middle one Home, the side ones
recent apps and the notifications; what you type goes to the phone (`-n`
only shows the screen), and the clipboard follows in both directions. The
phone's sound plays here (Android 11 or newer; `--no-audio` turns it off).
Alt+key (or Super+key) acts on the phone: `h` Home, `b` Back, `s` recent
apps, `m` Menu, `p` power, Up/Down volume, `n` notifications (with Shift,
close them), `r` rotate, `o` screen off (with Shift, on), `f` fullscreen.
The "Mirror the screen" action in the device list starts it too, and the
Mirroring tab of the settings has resolution, quality, frames per second,
codec and the rest (`[mirror]` in the .ini; the command line wins).

Without a cable: `kiremote-mirror --tcpip` switches the phone that is on USB
to Wi-Fi (`adb tcpip`) and uses it from then on, so the cable can go;
`--connect 192.168.0.7` (or `:PORT`) connects to a phone that is already
listening, such as one with Android's wireless debugging turned on. The
device list passes the phone's address it already knows (`--wifi`), used only
when no phone is on a cable.

The phone's camera as a webcam: `kiremote-mirror --camera` (or "Use as webcam"
in the device list) sends the picture of the phone's camera, without a window,
to a v4l2loopback video device, which browsers, video calls and OBS see as a
webcam (Android 12 or newer). The kernel module is yours to load, as root
(`sudo modprobe v4l2loopback video_nr=10 card_label=Kiremote
exclusive_caps=1`; the package is `v4l2loopback-dkms`): the Camera tab of the
settings says whether it is loaded and how to do it, and has the device, the
camera (back, front, external), the size and the left-right flip. DroidCam's
module (`v4l2loopback_dc`) is another one and is left alone. A phone is
taken by one `kiremote-mirror` at a time, mirrored or camera: asking again
brings the window forward, and the other use is refused with the reason.

If the computer doesn't show up on the phone, check that the firewall lets
UDP and TCP 1716 to 1764 through, or add the computer's IP in the app ("Add
devices by IP"). Kiremote can run next to KDE Connect on the same machine:
the phone sees them as two different computers.

## Install

Download the `.deb` packages for your distro from the
[Releases](https://github.com/kiyoshispreclerg/kiremote/releases) page
(Ubuntu 22.04, Ubuntu 24.04, Debian 12, Debian 13) and install them with
`sudo apt install ./kiremote_*.deb ./kiremote-gtk_*.deb`. Always `kiremote`
(the daemon and CLI), plus one interface:

- **KiDesktop:** `kiremote-kidesktop` (needs `kidesktop-core` 0.1.4 or newer).
- **Any other desktop:** `kiremote-gtk` (generic GTK 2 window and tray icon).
- Optional, for either: `kiremote-mirror` (screen mirroring; needs `adb`).

## Building

Needs a C11 compiler, `make`, `pkg-config`, OpenSSL and gettext (`msgfmt`).
`kiremote-mirror` is only built with libavcodec, libswscale, Xext, Xfixes and
libpulse (`libavcodec-dev libswscale-dev libxext-dev libxfixes-dev libpulse-dev`).

```sh
make
make test        # unit tests, fuzz corpus, and two daemons in network namespaces
build/kiremoted --help
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

Not affiliated with KDE. "KDE Connect" is only used to say which protocol
Kiremote is compatible with.

## Packages

One source, four `.deb` packages (Ubuntu 22.04/24.04, Debian 12/13):
`kiremote` (daemon and CLI), `kiremote-gtk` (the window, for any desktop) and
`kiremote-kidesktop` (kiconf tab, xisserve page and xispanel widget; needs
`kidesktop-core` 0.1.4 or newer) and `kiremote-mirror` (screen mirroring). `./scripts/install-build-deps.sh` and
`./scripts/build-deb.sh` build them into `dist/`.
