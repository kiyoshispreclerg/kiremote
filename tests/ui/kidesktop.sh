#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The three KiDesktop modules loaded by the real programs, in a private X
# server, with a private runtime/config/home and a real kiremoted in a
# network namespace of its own: the kiconf tab, the xisserve page and the
# xispanel widget (which asks a pairing question with `xisserve --question`).
#
#   tests/ui/kidesktop.sh BUILD_DIR
#
# Skips without kiconf, xisserve, xispanel, Xvfb or the built modules.
set -eu

if [ "${1:-}" != --inside ]; then
    build=$(cd "${1:-build}" && pwd)
    for tool in Xvfb kiconf xisserve xispanel; do
        command -v "$tool" >/dev/null 2>&1 || { echo "tests/ui/kidesktop.sh: skipped (no $tool)"; exit 0; }
    done
    for m in tabs/kiremote-tab.so pages/kiremote.so widgets/kiremote.so; do
        [ -e "$build/kidesktop/$m" ] || { echo "tests/ui/kidesktop.sh: skipped (no $m)"; exit 0; }
    done
    unshare -rn true 2>/dev/null || { echo "tests/ui/kidesktop.sh: skipped (no network namespaces)"; exit 0; }
    exec unshare -rn sh "$0" --inside "$build"
fi
build=$2
unset DISPLAY XAUTHORITY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS
tmp=$(mktemp -d /tmp/kiremote-kd-XXXXXX)
pids=""
cleanup() {
    for p in $pids; do kill "$p" 2>/dev/null || true; done
    # the question popup the widget starts is not ours to track by pid
    pkill -f "Pedinte-$$" 2>/dev/null || true
    wait 2>/dev/null || true
    if [ "${keep:-}" ]; then echo "logs kept in $tmp"; else rm -rf "$tmp"; fi
}
trap cleanup EXIT
fail() {
    echo "FAIL: $*" >&2
    for l in "$tmp"/*.log; do echo "--- $l"; cat "$l"; done >&2
    keep=1
    exit 1
}
wait_for() { # seconds command...
    t=$(($1 * 10)); shift
    while [ "$t" -gt 0 ]; do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
        t=$((t - 1))
    done
    return 1
}

mkdir -p "$tmp/run" "$tmp/cfg" "$tmp/cache" "$tmp/data"
chmod 700 "$tmp/run"
export HOME=$tmp XDG_CONFIG_HOME=$tmp/cfg XDG_CACHE_HOME=$tmp/cache XDG_DATA_HOME=$tmp/data \
    XDG_RUNTIME_DIR=$tmp/run LC_ALL=C.UTF-8 LANGUAGE=en NO_AT_BRIDGE=1
export XISPANEL_WIDGET_PATH=$build/kidesktop/widgets XISSERVE_PAGE_PATH=$build/kidesktop/pages \
    KICONF_TABS_DIR=$build/kidesktop/tabs KIREMOTE_LOCALEDIR=$build/locale

n=40
while [ -e "/tmp/.X11-unix/X$n" ] || [ -e "/tmp/.X$n-lock" ]; do n=$((n + 1)); done
Xvfb ":$n" -screen 0 800x400x24 -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
pids="$pids $!"
wait_for 5 test -e "/tmp/.X11-unix/X$n" || fail "Xvfb did not start"
export DISPLAY=":$n"

printf '[device]\nname = Mesa\n' >"$tmp/kiremote.ini"
"$build/kiremoted" -v -c "$tmp/kiremote.ini" >"$tmp/daemon.log" 2>&1 &
daemon=$!
pids="$pids $daemon"
wait_for 5 test -S "$tmp/run/kiremote.sock" || fail "the daemon did not start"
subscribers() { grep -c "control client subscribed" "$tmp/daemon.log"; }
has_subscribers() { [ "$(subscribers)" -ge "$1" ]; }

# ---- the kiconf tab: kiconf loads the module and the module reaches the daemon ----
kiconf --tab kiremote >"$tmp/kiconf.log" 2>&1 &
kiconf_pid=$!
pids="$pids $kiconf_pid"
wait_for 15 has_subscribers 1 || fail "the kiconf tab never reached the daemon"
kill -0 "$kiconf_pid" 2>/dev/null || fail "kiconf is gone"
grep -qi 'cannot load\|not loaded\|undefined symbol' "$tmp/kiconf.log" && fail "kiconf could not load the module"
echo "kiconf tab loaded"
kill "$kiconf_pid"

# ---- the xisserve page --------------------------------------------------------------
before=$(subscribers)
xisserve --kiremote >"$tmp/xisserve.log" 2>&1 &
xisserve_pid=$!
pids="$pids $xisserve_pid"
wait_for 15 has_subscribers $((before + 1)) || fail "the xisserve page never reached the daemon"
grep -q 'not loaded' "$tmp/xisserve.log" && fail "xisserve could not load the page"
echo "xisserve page loaded"
kill "$xisserve_pid" 2>/dev/null || true

# ---- the xispanel widget: a pairing question -----------------------------------------
printf 'PANEL\ttop\t*\tedge=top\tthickness=32\tmode=dock\nWIDGET\ttop\t0\tspacer\t\nWIDGET\ttop\t1\tkiremote\ticon=phone\n' >"$tmp/cfg/xispanel.conf"
# The daemon would write the status file itself; here one written by hand says
# a phone asks to pair (the pid is a live process: the file is believed).
kill "$daemon"
wait "$daemon" 2>/dev/null || true
sleep 300 &
alive=$!
pids="$pids $alive"
printf '{"pid":%d,"version":"t","id":"me","name":"Mesa","type":"desktop","devices":[{"id":"p1","name":"Pedinte-%s","type":"phone","reachable":true,"paired":false,"state":"requested_by_peer","key":"AABBCCDD"}]}' \
    "$alive" "$$" >"$tmp/run/kiremote-status.json"
xispanel >"$tmp/xispanel.log" 2>&1 &
pids="$pids $!"
wait_for 15 test -e "$tmp/run/kiremote-asked-p1-AABBCCDD" || fail "the widget did not ask about the pairing request"
grep -q 'kiremote' "$tmp/xispanel.log" && grep -qi 'not loaded\|failed to load\|unknown widget' "$tmp/xispanel.log" && fail "xispanel could not load the widget"
wait_for 5 pgrep -f "xisserve --question .*Pedinte-$$" || fail "no question popup"
echo "xispanel widget asked the question"
xispanel --quit >/dev/null 2>&1 || true

echo "tests/ui/kidesktop.sh: all passed"
