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

Early development: only the groundwork (phase 0). See
[PLANO.md](PLANO.md) (in Portuguese) for what is planned and in which order.

## Building

Needs a C11 compiler, `make`, `pkg-config`, OpenSSL and gettext (`msgfmt`).

```sh
make
make test
build/kiremoted --help
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

Not affiliated with KDE. "KDE Connect" is only used to say which protocol
Kiremote is compatible with.
