#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# libkrui's widgets in a private X server (never the user's display):
#
#   tests/ui/run.sh BUILD_DIR
#
# Skips without Xvfb.
set -eu
build=$(cd "${1:-build}" && pwd)
if ! command -v Xvfb >/dev/null 2>&1; then
    echo "tests/ui/run.sh: skipped (no Xvfb)"
    exit 0
fi
unset DISPLAY XAUTHORITY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS
export LC_ALL=C LANGUAGE=en NO_AT_BRIDGE=1 GTK_MODULES= GDK_NATIVE_WINDOWS=1

tmp=$(mktemp -d /tmp/kiremote-ui-XXXXXX)
xpid=""
cleanup() {
    [ -n "$xpid" ] && kill "$xpid" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

# The first free display from :40 up (what the other tests use too).
n=40
while [ -e "/tmp/.X11-unix/X$n" ] || [ -e "/tmp/.X$n-lock" ]; do n=$((n + 1)); done
Xvfb ":$n" -screen 0 800x600x24 -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
xpid=$!
t=50
while [ ! -e "/tmp/.X11-unix/X$n" ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
[ -e "/tmp/.X11-unix/X$n" ] || { echo "Xvfb did not start"; cat "$tmp/xvfb.log"; exit 1; }
export DISPLAY=":$n" XDG_CONFIG_HOME="$tmp/cfg" HOME="$tmp"

# Under ASan, GTK and fontconfig report what they never free (their
# singletons). That is not ours to fix and a suppression for the libraries
# would hide our own leaks (they pass through glib too), so a leak report
# only fails the test if one of its stacks goes through our code.
rc=0
LSAN_OPTIONS="${LSAN_OPTIONS:+$LSAN_OPTIONS:}exitcode=0" "$build/tests/test_ui" >"$tmp/out" 2>&1 || rc=$?
if awk 'BEGIN { RS = "" } /leak of/ && /(ui\/gtk\/|tests\/ui\/)/ { bad = 1 } END { exit !bad }' "$tmp/out"; then
    cat "$tmp/out"
    echo "tests/ui/run.sh: a leak in the interface code"
    exit 1
fi
sed '/ERROR: LeakSanitizer/,$d' "$tmp/out"
[ "$rc" -eq 0 ] || { echo "tests/ui/run.sh: FAILED"; exit "$rc"; }

# The program itself: a second kiremote shows the first one and exits.
if [ -x "$build/kiremote" ]; then
    mkdir -p "$tmp/run"
    export XDG_RUNTIME_DIR="$tmp/run"
    "$build/kiremote" -s "$tmp/run/k.sock" >"$tmp/first.log" 2>&1 &
    first=$!
    t=50
    while [ ! -S "$tmp/run/k-ui.sock" ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
    [ -S "$tmp/run/k-ui.sock" ] || { echo "the first kiremote did not start"; cat "$tmp/first.log"; kill $first; exit 1; }
    timeout 5 "$build/kiremote" -s "$tmp/run/k.sock" --settings >"$tmp/second.log" 2>&1 \
        || { echo "the second kiremote did not exit by itself"; cat "$tmp/second.log"; kill $first; exit 1; }
    kill -0 $first 2>/dev/null || { echo "the first kiremote is gone"; cat "$tmp/first.log"; exit 1; }
    kill $first
    wait $first 2>/dev/null || true
    [ ! -e "$tmp/run/k-ui.sock" ] || { echo "the socket was left behind after SIGTERM"; exit 1; }
    echo "tests/ui/run.sh: one window per session"
fi
echo "tests/ui/run.sh: all passed"
