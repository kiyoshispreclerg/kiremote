#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The mirror's window and clipboard in a private X server (never the user's
# display):
#
#   tests/mirror/run.sh BUILD_DIR
#
# Skips without Xvfb or xsel.
set -eu
build=$(cd "${1:-build}" && pwd)
for tool in Xvfb xsel; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "tests/mirror/run.sh: skipped (no $tool)"
        exit 0
    fi
done
unset DISPLAY XAUTHORITY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS
export LC_ALL=C.UTF-8 LANGUAGE=en

tmp=$(mktemp -d /tmp/kiremote-mirror-XXXXXX)
xpid=""
cleanup() {
    [ -n "$xpid" ] && kill "$xpid" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

n=40
while [ -e "/tmp/.X11-unix/X$n" ] || [ -e "/tmp/.X$n-lock" ]; do n=$((n + 1)); done
Xvfb ":$n" -screen 0 800x600x24 -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
xpid=$!
t=50
while [ ! -e "/tmp/.X11-unix/X$n" ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
[ -e "/tmp/.X11-unix/X$n" ] || { echo "Xvfb did not start"; cat "$tmp/xvfb.log"; exit 1; }
export DISPLAY=":$n" HOME="$tmp"

"$build/tests/test_screen"
