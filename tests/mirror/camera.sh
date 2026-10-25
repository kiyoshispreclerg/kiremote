#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# kiremote-mirror --camera against the fake adb: the server is asked for the
# camera, the frames come out in the (file standing for the) video device at
# the right size and flipped when asked, and a phone is taken by one process
# at a time, whatever it does with it. No X, no phone, no kernel module.
#
#   tests/mirror/camera.sh BUILD_DIR
set -eu
build=$(cd "${1:-build}" && pwd)
here=$(cd "$(dirname "$0")/../.." && pwd)
command -v python3 >/dev/null 2>&1 || { echo "tests/mirror/camera.sh: skipped (no python3)"; exit 0; }
[ -x "$build/kiremote-mirror" ] || { echo "tests/mirror/camera.sh: skipped (no kiremote-mirror)"; exit 0; }
unset DISPLAY XAUTHORITY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS PULSE_SERVER
export LC_ALL=C.UTF-8 LANGUAGE=en

tmp=$(mktemp -d /tmp/kiremote-camera-XXXXXX)
pids=""
cleanup() {
    for p in $pids; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT
fail() { echo "tests/mirror/camera.sh: FAIL: $*" >&2; exit 1; }

export XDG_RUNTIME_DIR="$tmp/run" HOME="$tmp" XDG_CONFIG_HOME="$tmp/cfg"
mkdir -m 700 "$tmp/run"
export KIREMOTE_ADB="$here/tests/tools/fake_adb" FAKEADB_LOG="$tmp/adb.log" \
    FAKEADB_STREAM="$here/tests/fixtures/scrcpy-video.bin" \
    KIREMOTE_SCRCPY_SERVER="$here/tests/fixtures/scrcpy-video.bin"
mirror="$build/kiremote-mirror -c $tmp/none.ini"

# Frames of 360x800 (the fixture's picture): 432000 bytes each in YU12.
frame=432000
waitfor() { # file bytes
    t=100
    while [ "$(stat -c %s "$1" 2>/dev/null || echo 0)" -lt "$2" ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
    [ "$(stat -c %s "$1" 2>/dev/null || echo 0)" -ge "$2" ] || return 1
}

# ---- the frames, and what the server was asked ----
KIREMOTE_V4L2_FILE="$tmp/plain.yuv" $mirror --camera --camera-facing front -v >"$tmp/plain.log" 2>&1 &
p=$!; pids="$pids $p"
waitfor "$tmp/plain.yuv" $((frame * 2)) || { cat "$tmp/plain.log"; fail "no frames"; }
kill $p; wait $p 2>/dev/null || true
grep -q 'video_source=camera' "$tmp/adb.log" || fail "the server was not asked for the camera"
grep -q 'camera_facing=front' "$tmp/adb.log" || fail "the facing was not passed"
grep -q 'control=false' "$tmp/adb.log" || fail "the camera must not open the control socket"
grep -q 'audio=false' "$tmp/adb.log" || fail "the camera must not ask for sound"
grep -q 'max_size=1280' "$tmp/adb.log" || fail "the default size cap was not passed"
size=$(stat -c %s "$tmp/plain.yuv")
[ $((size % frame)) -eq 0 ] || fail "the file is not whole frames ($size bytes)"

KIREMOTE_V4L2_FILE="$tmp/flip.yuv" $mirror --camera --flip >"$tmp/flip.log" 2>&1 &
p=$!; pids="$pids $p"
waitfor "$tmp/flip.yuv" $((frame * 2)) || { cat "$tmp/flip.log"; fail "no flipped frames"; }
kill $p; wait $p 2>/dev/null || true
python3 - "$tmp/plain.yuv" "$tmp/flip.yuv" <<'PY' || fail "the flipped frame is not the mirror of the plain one"
import sys
w, h = 360, 800
a = open(sys.argv[1], 'rb').read(w * h * 3 // 2)
b = open(sys.argv[2], 'rb').read(w * h * 3 // 2)
def rows(buf, off, pw, ph):
    return [buf[off + y * pw: off + (y + 1) * pw] for y in range(ph)]
for off, pw, ph in ((0, w, h), (w * h, w // 2, h // 2), (w * h * 5 // 4, w // 2, h // 2)):
    for ra, rb in zip(rows(a, off, pw, ph), rows(b, off, pw, ph)):
        if ra[::-1] != rb:
            sys.exit(1)
PY

# ---- one process per phone ----
rm -f "$tmp/cam.yuv"
KIREMOTE_V4L2_FILE="$tmp/cam.yuv" $mirror --camera --id 0123456789abcdef >"$tmp/first.log" 2>&1 &
first=$!; pids="$pids $first"
waitfor "$tmp/cam.yuv" $frame || { cat "$tmp/first.log"; fail "the first camera did not start"; }
[ -S "$tmp/run/kiremote-mirror/0123456789abcdef.sock" ] || fail "no socket for the phone"

rc=0
$mirror --id 0123456789abcdef >"$tmp/second.log" 2>&1 || rc=$?
[ "$rc" -eq 1 ] || { cat "$tmp/second.log"; fail "a mirror of a phone used as a camera: exit $rc, wanted 1"; }
grep -q 'being used as a webcam' "$tmp/second.log" || { cat "$tmp/second.log"; fail "no explanation for the screen"; }

rc=0
KIREMOTE_V4L2_FILE="$tmp/cam2.yuv" $mirror --camera --id 0123456789abcdef >"$tmp/third.log" 2>&1 || rc=$?
[ "$rc" -eq 0 ] || { cat "$tmp/third.log"; fail "a second camera of the same phone: exit $rc, wanted 0"; }
grep -q 'Already open' "$tmp/third.log" || fail "the second camera did not say it was already open"
[ ! -e "$tmp/cam2.yuv" ] || fail "the second camera wrote frames"
kill -0 $first 2>/dev/null || fail "the first camera is gone"

# The same phone through another path is the same phone: the id is the key.
kill $first; wait $first 2>/dev/null || true
t=30; while [ -S "$tmp/run/kiremote-mirror/0123456789abcdef.sock" ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
[ ! -S "$tmp/run/kiremote-mirror/0123456789abcdef.sock" ] || fail "the socket stayed after the camera ended"

# ---- the connection drops: over Wi-Fi it is tried again, on a cable it ends ----
starts() { grep -c 'video_source=camera' "$tmp/adb.log" || true; }
rm -f "$tmp"/adb.log*
: >"$tmp/adb.log"
FAKEADB_MODE=drop-once KIREMOTE_V4L2_FILE="$tmp/wifi.yuv" $mirror --camera --connect 192.168.1.42 -v >"$tmp/wifi.log" 2>&1 &
wifi=$!; pids="$pids $wifi"
t=100; while ! grep -q 'Lost the connection' "$tmp/wifi.log" && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
grep -q 'Lost the connection' "$tmp/wifi.log" || { cat "$tmp/wifi.log"; fail "the drop was not noticed"; }
first_run=$(stat -c %s "$tmp/wifi.yuv") # all of the first stream's frames: the fixture is finite
[ "$first_run" -ge "$frame" ] || fail "no frames before the drop"
t=100; while [ "$(starts)" -lt 2 ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
[ "$(starts)" -ge 2 ] || { cat "$tmp/wifi.log"; fail "the server was not started again after the drop"; }
waitfor "$tmp/wifi.yuv" $((first_run * 2)) || { cat "$tmp/wifi.log"; fail "no frames after the phone came back"; }
kill -0 $wifi 2>/dev/null || { cat "$tmp/wifi.log"; fail "the camera ended instead of reconnecting"; }
[ "$(grep -c '^connect 192.168.1.42' "$tmp/adb.log")" -ge 2 ] || fail "adb connect was not run again"
grep -q 'Lost the connection' "$tmp/wifi.log" || fail "the drop was not reported"
grep -q 'Got the phone back' "$tmp/wifi.log" || fail "the return was not reported"
kill $wifi; wait $wifi 2>/dev/null || true

rm -f "$tmp"/adb.log*
: >"$tmp/adb.log"
FAKEADB_MODE=drop-once KIREMOTE_V4L2_FILE="$tmp/usb.yuv" $mirror --camera >"$tmp/usb.log" 2>&1 &
usb=$!; pids="$pids $usb"
t=100; while kill -0 $usb 2>/dev/null && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
if kill -0 $usb 2>/dev/null; then cat "$tmp/usb.log"; fail "a dropped cable connection did not end the camera"; fi
wait $usb 2>/dev/null || true
[ "$(starts)" -eq 1 ] || fail "a cable connection was retried"

# ---- the video device is checked before the phone is touched ----
rc=0
$mirror --camera --v4l2 /nonexistent/video9 >"$tmp/nodev.log" 2>&1 || rc=$?
[ "$rc" -eq 1 ] || fail "a missing device: exit $rc"
grep -q 'does not exist' "$tmp/nodev.log" || { cat "$tmp/nodev.log"; fail "no explanation for the missing device"; }
echo "tests/mirror/camera.sh: all passed"
