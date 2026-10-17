#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The phase 2 plugins against a scripted phone (tests/tools/fakephone.c),
# in a network namespace, with a private session bus holding a stand-in
# notification server and an MPRIS player, and fake pactl/paplay on PATH.
# It never touches the user's bus, audio or notifications.
#
#   tests/net/plugins.sh BUILD_DIR
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
if [ "${1:-}" != --inside ]; then
    build=$(cd "${1:-build}" && pwd)
    if ! unshare -rn true 2>/dev/null; then
        echo "tests/net/plugins.sh: skipped (no unprivileged network namespaces)"
        exit 0
    fi
    for tool in dbus-daemon python3; do
        command -v $tool >/dev/null || { echo "tests/net/plugins.sh: skipped (no $tool)"; exit 0; }
    done
    python3 -c 'import dbus, gi' 2>/dev/null ||
        { echo "tests/net/plugins.sh: skipped (no python dbus/gi)"; exit 0; }
    exec unshare -rn sh "$0" --inside "$build"
fi
build=$2
export LC_ALL=C LANGUAGE=en

tmp=$(mktemp -d /tmp/kiremote-plugins-XXXXXX)
pids=""
cleanup() {
    exec 7>&- 2>/dev/null || true
    for p in $pids; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    if [ "${keep:-}" ]; then echo "logs kept in $tmp"; else rm -rf "$tmp"; fi
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    for l in "$tmp"/*.log "$tmp"/*.out "$tmp"/pactl/log "$tmp"/play.log; do
        [ -f "$l" ] && { echo "--- $l"; cat "$l"; }
    done >&2
    keep=1
    exit 1
}

wait_for() {
    t=$(($1 * 10)); shift
    while [ "$t" -gt 0 ]; do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
        t=$((t - 1))
    done
    return 1
}
has() { grep -q -- "$2" "$1"; }
count() { grep -c -- "$2" "$1" 2>/dev/null || true; }

# ---- the fake world ---------------------------------------------------------------
ip link set lo up
mkdir -p "$tmp/cfg" "$tmp/run" "$tmp/share" "$tmp/bin" "$tmp/pactl" "$tmp/phone" "$tmp/power/BAT0" "$tmp/power/mouse"
ln -s "$here/tools/fake_pactl" "$tmp/bin/pactl"
ln -s "$here/tools/fake_paplay" "$tmp/bin/paplay"
export PATH="$tmp/bin:$PATH" FAKE_PACTL_DIR="$tmp/pactl" FAKE_PLAY_LOG="$tmp/play.log"
: >"$tmp/play.log"

# A laptop battery (and a mouse's, which must not count).
printf 'Battery\n' >"$tmp/power/BAT0/type"; echo 80 >"$tmp/power/BAT0/capacity"; echo Discharging >"$tmp/power/BAT0/status"
printf 'Battery\n' >"$tmp/power/mouse/type"; echo Device >"$tmp/power/mouse/scope"; echo 5 >"$tmp/power/mouse/capacity"

cat >"$tmp/pactl/sinks.json" <<'JSON'
[{"index":0,"name":"sink-a","description":"Speakers","mute":false,
  "volume":{"front-left":{"value":16373},"front-right":{"value":20000}}},
 {"index":1,"name":"sink-b","description":"(null)","mute":true,
  "volume":{"front-left":{"value":65536}},"properties":{"device.description":"Headphones"}}]
JSON
echo sink-a >"$tmp/pactl/default"
: >"$tmp/pactl/events"; : >"$tmp/pactl/log"
echo "Mute: yes" >"$tmp/pactl/mute"

dbus-daemon --session --nofork --nopidfile --address="unix:path=$tmp/bus" >"$tmp/dbus.log" 2>&1 &
pids="$pids $!"
wait_for 5 test -S "$tmp/bus" || fail "no private bus"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/bus"

: >"$tmp/notes.cmd"; : >"$tmp/player.cmd"
python3 "$here/tools/mock_notifications.py" "$tmp/notes.log" "$tmp/notes.cmd" &
pids="$pids $!"
python3 "$here/tools/mock_player.py" testplayer "$tmp/player.log" "$tmp/player.cmd" &
pids="$pids $!"
wait_for 10 has "$tmp/notes.log" READY || fail "notification mock did not start"
wait_for 10 has "$tmp/player.log" READY || fail "player mock did not start"

printf '[device]\nname = Alfa\n[share]\ndir = %s\n[battery]\npower_supply_dir = %s\n' \
    "$tmp/share" "$tmp/power" >"$tmp/kiremote.ini"
XDG_CONFIG_HOME=$tmp/cfg XDG_RUNTIME_DIR=$tmp/run HOME=$tmp \
    "$build/kiremoted" -v -c "$tmp/kiremote.ini" >"$tmp/a.log" 2>&1 &
daemon=$!
pids="$pids $daemon"
wait_for 5 test -S "$tmp/run/kiremote.sock" || fail "the daemon did not start"
wait_for 5 has "$tmp/a.log" "this is Alfa" || fail "the daemon's log"
id_a=$(sed -n 's/.*this is Alfa.*id \([0-9a-f]*\),.*/\1/p' "$tmp/a.log")
cli() { "$build/kiremote-cli" -s "$tmp/run/kiremote.sock" "$@"; }

# ---- the phone ------------------------------------------------------------------------
IN="kdeconnect.ping,kdeconnect.share.request,kdeconnect.battery,kdeconnect.findmyphone.request,kdeconnect.telephony.request_mute,kdeconnect.mpris,kdeconnect.systemvolume"
OUT="kdeconnect.ping,kdeconnect.share.request,kdeconnect.battery,kdeconnect.connectivity_report,kdeconnect.findmyphone.request,kdeconnect.telephony,kdeconnect.mpris.request,kdeconnect.systemvolume.request"
mkfifo "$tmp/phone.in"
"$build/tests/fakephone" -d "$tmp/phone" -t 127.0.0.1:1716 -i "$id_a" -n "Fake Phone" -I "$IN" -O "$OUT" \
    <>"$tmp/phone.in" >"$tmp/phone.out" 2>&1 &
pids="$pids $!"
exec 7>"$tmp/phone.in"
send() { printf '%s\n' "$1" >&7; }
phone_has() { has "$tmp/phone.out" "$1"; }
wait_for 10 phone_has READY || fail "the phone did not connect"
id_p=$(sed -n 's/^ID //p' "$tmp/phone.out")

send pair
wait_for 5 has "$tmp/a.log" "wants to pair" || fail "no pairing request"
cli -d "$id_p" --pair >"$tmp/pair.out" 2>&1 || fail "pairing: $(cat "$tmp/pair.out")"
wait_for 5 phone_has '"pair":true' || fail "the phone was not told it is paired"
echo "paired with the fake phone"

# ---- ping: a notification --------------------------------------------------------------
send '{"type":"kdeconnect.ping","body":{}}'
wait_for 5 has "$tmp/notes.log" "NOTIFY 1 | Fake Phone | Ping!" || fail "ping notification"
send '{"type":"kdeconnect.ping","body":{"message":"oi"}}'
wait_for 5 has "$tmp/notes.log" "| Fake Phone | oi |" || fail "ping with a message"

# ---- battery: ours goes out on connect, the mouse's does not count -----------------------
wait_for 5 phone_has '"currentCharge":80' || fail "our battery was not sent"
phone_has '"thresholdEvent":0' || fail "threshold"
phone_has '"batteryQuantity":1' || fail "the mouse was counted as a battery"
echo 14 >"$tmp/power/BAT0/capacity"
send '{"type":"kdeconnect.battery","body":{"request":true}}'
wait_for 5 phone_has '"currentCharge":14,"batteryQuantity":1,"thresholdEvent":1' ||
    wait_for 2 phone_has '"currentCharge":14' || fail "request did not get the new level"
phone_has '"thresholdEvent":1' || fail "low threshold"
# ... and the phone's, with a low-battery notification
send '{"type":"kdeconnect.battery","body":{"isCharging":false,"currentCharge":9,"thresholdEvent":1}}'
wait_for 5 has "$tmp/notes.log" "Fake Phone: Low Battery | Battery at 9%" || fail "low battery notification"
wait_for 5 sh -c "'$build/kiremote-cli' -s '$tmp/run/kiremote.sock' -l | grep -q 'battery 9%'" || fail "battery in the list"

# ---- connectivity report ----------------------------------------------------------------------
send '{"type":"kdeconnect.connectivity_report","body":{"signalStrengths":{"6":{"networkType":"LTE","signalStrength":3},"17":{"networkType":"HSPA","signalStrength":1}}}}'
wait_for 5 sh -c "'$build/kiremote-cli' -s '$tmp/run/kiremote.sock' -l | grep -q 'signal LTE 3/4'" || fail "signal in the list"

# ---- find my phone ------------------------------------------------------------------------------
cli -d "Fake Phone" --find-phone || fail "find-phone"
wait_for 5 phone_has '"type":"kdeconnect.findmyphone.request"' || fail "the phone was not asked to ring"

# ---- find this computer: sound, unmute, stop by the notification -----------------------------------
send '{"type":"kdeconnect.findmyphone.request","body":{}}'
wait_for 5 has "$tmp/notes.log" "Fake Phone is looking for this computer |  | stop,Stop | timeout=0 | urgency=2" ||
    fail "find notification"
wait_for 5 has "$tmp/play.log" "play" || fail "no sound played"
has "$tmp/pactl/log" "set-sink-mute @DEFAULT_SINK@ 0" || fail "the muted output was not unmuted"
nid=$(sed -n 's/^NOTIFY \([0-9]*\) | Fake Phone is looking.*/\1/p' "$tmp/notes.log")
sleep 1.2
[ "$(count "$tmp/play.log" play)" -ge 2 ] || fail "the sound did not repeat"
echo "action $nid stop" >>"$tmp/notes.cmd"
wait_for 5 has "$tmp/notes.log" "CLOSE $nid" || fail "the notification was not closed"
wait_for 5 has "$tmp/pactl/log" "set-sink-mute @DEFAULT_SINK@ 1" || fail "the output was not muted again"
n=$(count "$tmp/play.log" play); sleep 1.2
[ "$(count "$tmp/play.log" play)" -le $((n + 1)) ] || fail "the sound kept playing after stop"

# ---- telephony: ringing, mute button, cancel as a string ------------------------------------------------
send '{"type":"kdeconnect.telephony","body":{"event":"ringing","phoneNumber":"+5511999","contactName":"Maria"}}'
wait_for 5 has "$tmp/notes.log" "| Fake Phone | Incoming call from Maria | mute,Mute Call | timeout=0 | urgency=2" ||
    fail "ringing notification"
cid=$(sed -n 's/^NOTIFY \([0-9]*\) | Fake Phone | Incoming call from Maria.*/\1/p' "$tmp/notes.log")
echo "action $cid mute" >>"$tmp/notes.cmd"
wait_for 5 phone_has '"type":"kdeconnect.telephony.request_mute"' || fail "mute was not sent"
send '{"type":"kdeconnect.telephony","body":{"event":"ringing","phoneNumber":"+5511999","isCancel":"true"}}'
wait_for 5 has "$tmp/notes.log" "CLOSE $cid" || fail "the call notification was not closed"
send '{"type":"kdeconnect.telephony","body":{"event":"missedCall","phoneNumber":"+5511888"}}'
wait_for 5 has "$tmp/notes.log" "Missed call from +5511888" || fail "missed call"
cli -d "Fake Phone" --mute-call || fail "mute-call request"

# ---- pause music while the call rings, resume after -----------------------------------------------------
echo "status Playing" >>"$tmp/player.cmd"
sleep 0.5
send '{"type":"kdeconnect.telephony","body":{"event":"ringing","phoneNumber":"+5511777"}}'
wait_for 5 has "$tmp/player.log" "CALL Pause" || fail "the player was not paused"
send '{"type":"kdeconnect.telephony","body":{"event":"ringing","phoneNumber":"+5511777","isCancel":"true"}}'
wait_for 5 has "$tmp/player.log" "CALL Play" || fail "the player was not resumed"
# a player that was already paused is left alone
echo "status Paused" >>"$tmp/player.cmd"
sleep 0.5
n=$(count "$tmp/player.log" "CALL Play")
send '{"type":"kdeconnect.telephony","body":{"event":"ringing","phoneNumber":"+5511666"}}'
send '{"type":"kdeconnect.telephony","body":{"event":"ringing","phoneNumber":"+5511666","isCancel":"true"}}'
sleep 0.6
[ "$(count "$tmp/player.log" "CALL Play")" -eq "$n" ] || fail "resumed a player that was not playing"

# ---- media control ---------------------------------------------------------------------------------------
send '{"type":"kdeconnect.mpris.request","body":{"requestPlayerList":true}}'
wait_for 5 phone_has '"playerList":\["Test Player"\]' || fail "player list"
phone_has '"supportAlbumArtPayload":true' || fail "album art flag"
send '{"type":"kdeconnect.mpris.request","body":{"player":"Test Player","requestNowPlaying":true,"requestVolume":true}}'
wait_for 5 phone_has '"title":"Song"' || fail "now playing"
phone_has '"artist":"Band"' && phone_has '"length":180000' && phone_has '"pos":3000' &&
    phone_has '"volume":50' && phone_has '"canGoNext":true' || fail "now playing fields"
send '{"type":"kdeconnect.mpris.request","body":{"player":"Test Player","action":"Next"}}'
wait_for 5 has "$tmp/player.log" "CALL Next" || fail "Next"
send '{"type":"kdeconnect.mpris.request","body":{"player":"Test Player","action":"Quit"}}'
send '{"type":"kdeconnect.mpris.request","body":{"player":"Test Player","setVolume":30}}'
wait_for 5 has "$tmp/player.log" "SET Volume 0.3" || fail "setVolume"
has "$tmp/player.log" "CALL Quit" && fail "an action outside the list was executed"
send '{"type":"kdeconnect.mpris.request","body":{"player":"Test Player","SetPosition":10000}}'
wait_for 5 has "$tmp/player.log" "SEEK 7000000" || fail "SetPosition"
send '{"type":"kdeconnect.mpris.request","body":{"player":"Test Player","Seek":-2000000}}'
wait_for 5 has "$tmp/player.log" "SEEK -2000000" || fail "Seek"
echo "title Novo" >>"$tmp/player.cmd"
wait_for 5 phone_has '"title":"Novo"' || fail "metadata change was not pushed"
echo "status Paused" >>"$tmp/player.cmd"
wait_for 5 phone_has '"isPlaying":false' || fail "status change was not pushed"

# ---- volume --------------------------------------------------------------------------------------------------
has "$tmp/pactl/log" subscribe || fail "pactl subscribe is not running while a phone is connected"
send '{"type":"kdeconnect.systemvolume.request","body":{"requestSinks":true}}'
wait_for 5 phone_has '"sinkList"' || fail "sink list"
phone_has '"name":"sink-a","muted":false,"description":"Speakers","volume":20000,"maxVolume":65536,"enabled":true' ||
    fail "sink a"
phone_has '"name":"sink-b","muted":true,"description":"Headphones","volume":65536,"maxVolume":65536,"enabled":false' ||
    fail "sink b (description from the property)"
send '{"type":"kdeconnect.systemvolume.request","body":{"name":"sink-a","volume":30000}}'
wait_for 5 has "$tmp/pactl/log" "set-sink-volume sink-a 30000" || fail "set volume"
wait_for 5 has "$tmp/pactl/log" "set-sink-mute sink-a 0" || fail "volume change should unmute"
send '{"type":"kdeconnect.systemvolume.request","body":{"name":"sink-b","muted":false}}'
wait_for 5 has "$tmp/pactl/log" "set-sink-mute sink-b 0" || fail "set mute"
send '{"type":"kdeconnect.systemvolume.request","body":{"name":"sink-b","enabled":true}}'
wait_for 5 has "$tmp/pactl/log" "set-default-sink sink-b" || fail "set default"
send '{"type":"kdeconnect.systemvolume.request","body":{"name":"--evil","volume":1}}'
sleep 0.5
has "$tmp/pactl/log" "evil" && fail "a name that is not a sink reached pactl"
# a change made elsewhere is pushed
sed -i 's/"value":16373/"value":40000/; s/"value":20000/"value":40000/' "$tmp/pactl/sinks.json"
echo "Event 'change' on sink #0" >>"$tmp/pactl/events"
wait_for 5 phone_has '"volume":40000,"name":"sink-a"' || fail "volume change was not pushed"

# ---- clean exit ------------------------------------------------------------------------------------------------
exec 7>&-
kill "$daemon"
wait "$daemon" 2>/dev/null || true
pids=$(echo "$pids" | sed "s/ $daemon//")
if grep -q "Sanitizer\|runtime error" "$tmp/a.log"; then fail "sanitizer report"; fi

echo "tests/net/plugins.sh: all passed"
