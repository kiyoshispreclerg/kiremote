#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The desktop-facing plugins against a scripted phone, in a network namespace:
# clipboard, shared text and links, the phone's notifications, commands,
# mouse and keyboard, the presentation pointer, keeping the screen awake and
# locking. The X server is a private Xvfb, the session and the "system" bus
# are private dbus-daemons with stand-in services, and xdg-open, pactl, paplay
# and xdg-screensaver are fakes on PATH: nothing here touches the user's
# desktop, clipboard, pointer, audio or notifications.
#
#   tests/net/desktop.sh BUILD_DIR
#
# The test X server is an Xvfb. KR_TEST_XEPHYR=1 makes it a Xephyr window on
# the display this is run from, to watch the pointer move and the keys being
# typed (the daemon only ever talks to the nested server). Off by default:
# a window that appears on someone's session, which may itself be a nested
# one, is not something a test should do uninvited.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
if [ "${1:-}" != --inside ]; then
    build=$(cd "${1:-build}" && pwd)
    if ! unshare -rn true 2>/dev/null; then
        echo "tests/net/desktop.sh: skipped (no unprivileged network namespaces)"
        exit 0
    fi
    for tool in dbus-daemon python3 Xvfb xsel; do
        command -v $tool >/dev/null || { echo "tests/net/desktop.sh: skipped (no $tool)"; exit 0; }
    done
    python3 -c 'import dbus, gi' 2>/dev/null ||
        { echo "tests/net/desktop.sh: skipped (no python dbus/gi)"; exit 0; }
    # The host display, only for Xephyr to draw its window on.
    KR_HOST_DISPLAY=${DISPLAY:-} KR_HOST_XAUTHORITY=${XAUTHORITY:-} \
        exec unshare -rn sh "$0" --inside "$build"
fi
build=$2
export LC_ALL=C LANGUAGE=en
# Never the user's desktop: these sockets are files, reachable from the
# namespace. Everything below starts its own.
unset DISPLAY XAUTHORITY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS

tmp=$(mktemp -d /tmp/kiremote-desktop-XXXXXX)
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
    for l in "$tmp"/*.log "$tmp"/*.out "$tmp"/xdg-open "$tmp"/xdgss "$tmp"/lock.ran; do
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
has() { grep -aq -- "$2" "$1"; }
count() { grep -ac -- "$2" "$1" 2>/dev/null || true; }

# ---- the fake world ---------------------------------------------------------------
ip link set lo up
mkdir -p "$tmp/cfg" "$tmp/run" "$tmp/share" "$tmp/bin" "$tmp/pactl" "$tmp/phone" "$tmp/cache"
for f in pactl:fake_pactl paplay:fake_paplay xdg-screensaver:fake_xdg_screensaver; do
    ln -s "$here/tools/${f#*:}" "$tmp/bin/${f%%:*}"
done
printf '#!/bin/sh\necho "$*" >>"%s/xdg-open"\n' "$tmp" >"$tmp/bin/xdg-open"
chmod +x "$tmp/bin/xdg-open"
export PATH="$tmp/bin:$PATH" FAKE_PACTL_DIR="$tmp/pactl" FAKE_PLAY_LOG="$tmp/play.log" FAKE_XDGSS_LOG="$tmp/xdgss"
echo '[]' >"$tmp/pactl/sinks.json"; echo none >"$tmp/pactl/default"; : >"$tmp/pactl/events"; : >"$tmp/pactl/log"
: >"$tmp/play.log"; : >"$tmp/xdgss"; : >"$tmp/xdg-open"

# X: a private server on a high display number (from :40 up, the first free
# one), far from any real session.
n=40
while [ -e "/tmp/.X11-unix/X$n" ] || [ -e "/tmp/.X$n-lock" ]; do n=$((n + 1)); done
echo $n >"$tmp/display"
if [ -n "${KR_TEST_XEPHYR:-}" ] && [ -n "${KR_HOST_DISPLAY:-}" ] && command -v Xephyr >/dev/null; then
    echo "(showing the test desktop in a Xephyr window on $KR_HOST_DISPLAY, as :$n)"
    env DISPLAY="$KR_HOST_DISPLAY" ${KR_HOST_XAUTHORITY:+XAUTHORITY="$KR_HOST_XAUTHORITY"} \
        Xephyr ":$n" -screen 800x600 -title "kiremote test" -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
else
    Xvfb ":$n" -screen 0 800x600x24 -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
fi
pids="$pids $!"
wait_for 10 test -S "/tmp/.X11-unix/X$n" || fail "no X server on :$n"
export DISPLAY=":$(cat "$tmp/display")"
xtool="$build/tests/xtool"

# Two private buses: "session" and "system".
dbus-daemon --session --nofork --nopidfile --address="unix:path=$tmp/bus" >"$tmp/dbus.log" 2>&1 &
pids="$pids $!"
dbus-daemon --session --nofork --nopidfile --address="unix:path=$tmp/sysbus" >"$tmp/sysdbus.log" 2>&1 &
pids="$pids $!"
wait_for 5 test -S "$tmp/bus" || fail "no private session bus"
if [ -n "${KR_TEST_MONITOR:-}" ]; then # every message on the test bus, for debugging
    DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/bus" dbus-monitor --session >"$tmp/monitor.log" 2>&1 &
    pids="$pids $!"
fi
wait_for 5 test -S "$tmp/sysbus" || fail "no private system bus"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/bus" DBUS_SYSTEM_BUS_ADDRESS="unix:path=$tmp/sysbus"

: >"$tmp/notes.cmd"; : >"$tmp/login.cmd"
python3 "$here/tools/mock_notifications.py" "$tmp/notes.log" "$tmp/notes.cmd" &
pids="$pids $!"
python3 "$here/tools/mock_login1.py" "$tmp/login.cmd" &
pids="$pids $!"
python3 "$here/tools/mock_screensaver.py" "$tmp/ss.log" &
sspid=$!
pids="$pids $sspid"
wait_for 10 has "$tmp/notes.log" READY || fail "notification mock did not start"
wait_for 10 test -f "$tmp/login.cmd.ready" || fail "login1 mock did not start"
wait_for 10 has "$tmp/ss.log" READY || fail "screensaver mock did not start"

cat >"$tmp/kiremote.ini" <<EOF
[device]
name = Alfa
[share]
dir = $tmp/share
[plugins]
screensaver_inhibit = true
[lock]
command = echo lock >>$tmp/lock.ran
unlock_command = echo unlock >>$tmp/lock.ran
[runcommand]
Say hi = echo hello; echo world
Fail = echo oops >&2; exit 3
Sleeper = sleep 30
EOF
XDG_CONFIG_HOME=$tmp/cfg XDG_CACHE_HOME=$tmp/cache XDG_RUNTIME_DIR=$tmp/run HOME=$tmp \
    "$build/kiremoted" -v -c "$tmp/kiremote.ini" >"$tmp/a.log" 2>&1 &
daemon=$!
pids="$pids $daemon"
wait_for 5 test -S "$tmp/run/kiremote.sock" || fail "the daemon did not start"
wait_for 5 has "$tmp/a.log" "this is Alfa" || fail "the daemon's log"
id_a=$(sed -n 's/.*this is Alfa.*id \([0-9a-f]*\),.*/\1/p' "$tmp/a.log")
cli() { "$build/kiremote-cli" -s "$tmp/run/kiremote.sock" "$@"; }

# ---- the phone, paired ----------------------------------------------------------------------
IN="kdeconnect.ping,kdeconnect.share.request,kdeconnect.clipboard,kdeconnect.clipboard.connect,kdeconnect.mousepad.keyboardstate,kdeconnect.runcommand,kdeconnect.runcommand.output,kdeconnect.lock,kdeconnect.lock.request,kdeconnect.notification.request,kdeconnect.notification.action"
OUT="kdeconnect.ping,kdeconnect.share.request,kdeconnect.clipboard,kdeconnect.clipboard.connect,kdeconnect.mousepad.request,kdeconnect.runcommand.request,kdeconnect.lock,kdeconnect.lock.request,kdeconnect.notification,kdeconnect.presenter"
mkfifo "$tmp/phone.in"
start_phone() {
    : >"$tmp/phone.out"
    "$build/tests/fakephone" -d "$tmp/phone" -t 127.0.0.1:1716 -i "$id_a" -n "Fake Phone" -I "$IN" -O "$OUT" \
        <>"$tmp/phone.in" >"$tmp/phone.out" 2>&1 &
    phone=$!
    pids="$pids $phone"
    wait_for 10 phone_has READY || fail "the phone did not connect"
}
send() { printf '%s\n' "$1" >&7; }
phone_has() { has "$tmp/phone.out" "$1"; }
exec 7<>"$tmp/phone.in"
start_phone
id_p=$(sed -n 's/^ID //p' "$tmp/phone.out")
send pair
wait_for 5 has "$tmp/a.log" "wants to pair" || fail "no pairing request"
cli -d "$id_p" --pair >"$tmp/pair.out" 2>&1 || fail "pairing: $(cat "$tmp/pair.out")"
wait_for 5 phone_has '"pair":true' || fail "the phone was not told it is paired"
echo "paired with the fake phone"

# ---- clipboard ---------------------------------------------------------------------------------
phone_has '"state":true' || wait_for 5 phone_has '"state":true' || fail "keyboard state"
send '{"type":"kdeconnect.clipboard","body":{"content":"olá mundo"}}'
wait_for 5 sh -c "[ \"\$(xsel -b -o)\" = 'olá mundo' ]" || fail "phone text did not reach the clipboard"
printf 'copiado aqui' | xsel -b -i
wait_for 5 phone_has '"content":"copiado aqui"' || fail "desktop text did not reach the phone"
# a password manager's secret is not shared
"$xtool" own "segredo" --secret >"$tmp/own.out" 2>&1 &
ownpid=$!
pids="$pids $ownpid"
wait_for 5 has "$tmp/own.out" owning || fail "xtool did not take the clipboard"
sleep 1
phone_has segredo && fail "a password-manager secret was shared"
kill "$ownpid"
# the connection packet: older than ours is ignored, newer wins
send '{"type":"kdeconnect.clipboard.connect","body":{"content":"velho","timestamp":1}}'
sleep 0.5
[ "$(xsel -b -o)" != velho ] || fail "an older clipboard replaced ours"
send '{"type":"kdeconnect.clipboard.connect","body":{"content":"novo","timestamp":99999999999999}}'
wait_for 5 sh -c "[ \"\$(xsel -b -o)\" = novo ]" || fail "a newer clipboard was ignored"
send '{"type":"kdeconnect.clipboard.connect","body":{"content":"sem tempo","timestamp":0}}'
sleep 0.5
[ "$(xsel -b -o)" = novo ] || fail "a clipboard with timestamp 0 was applied"
echo "clipboard ok"

# ---- shared text and links ---------------------------------------------------------------------
send '{"type":"kdeconnect.share.request","body":{"text":"veja isto\nhttps://exemplo.org/x"}}'
wait_for 5 has "$tmp/notes.log" "Shared text from Fake Phone copied to clipboard | veja isto" || fail "text notification"
has "$tmp/notes.log" "open,Open Link" || fail "no Open Link button for a text ending in a link"
[ "$(xsel -b -o)" = "$(printf 'veja isto\nhttps://exemplo.org/x')" ] || fail "shared text is not on the clipboard"
nid=$(sed -n 's/^NOTIFY \([0-9]*\) | Shared text from.*/\1/p' "$tmp/notes.log" | tail -1)
echo "action $nid open" >>"$tmp/notes.cmd"
wait_for 5 has "$tmp/xdg-open" "https://exemplo.org/x" || fail "Open Link did not open the link"
: >"$tmp/xdg-open"
send '{"type":"kdeconnect.share.request","body":{"url":"https://exemplo.org/y"}}'
wait_for 5 has "$tmp/xdg-open" "https://exemplo.org/y" || fail "a shared link was not opened"
send '{"type":"kdeconnect.share.request","body":{"url":"file:///etc/passwd"}}'
send '{"type":"kdeconnect.share.request","body":{"url":"javascript:alert(1)"}}'
send '{"type":"kdeconnect.share.request","body":{"url":"https://exemplo.org/z w"}}'
sleep 0.7
[ "$(count "$tmp/xdg-open" '')" -eq 1 ] || fail "a link with an unknown scheme (or spaces) was opened: $(cat "$tmp/xdg-open")"
cli -d "Fake Phone" --share-text "do computador" || fail "share-text"
cli -d "Fake Phone" --share-url "https://exemplo.org/w" || fail "share-url"
wait_for 5 phone_has '"text":"do computador"' || fail "share-text did not arrive"
wait_for 5 phone_has '"url":"https://exemplo.org/w"' || fail "share-url did not arrive"
echo "share ok"

# ---- the phone's notifications --------------------------------------------------------------------
printf 'PNGDATA-not-really' >"$tmp/icon.png"
send "payload $tmp/icon.png"' {"type":"kdeconnect.notification","body":{"id":"n1","appName":"Chat","title":"Maria","text":"oi","isClearable":true,"actions":["Responder"],"payloadHash":"abc123"}}'
wait_for 5 has "$tmp/notes.log" "| Chat | Maria: oi | a0,Responder | timeout=-1 | urgency=1 | replaces=0 | icon=$tmp/cache/kiremote/icons/abc123" ||
    fail "notification with its icon"
cmp -s "$tmp/icon.png" "$tmp/cache/kiremote/icons/abc123" || fail "the icon was not cached intact"
[ "$(stat -c %a "$tmp/cache/kiremote/icons")" = 700 ] || fail "the icon cache is not private"
nid=$(sed -n 's/^NOTIFY \([0-9]*\) | Chat | Maria: oi.*/\1/p' "$tmp/notes.log" | head -1)
send '{"type":"kdeconnect.notification","body":{"id":"n1","appName":"Chat","title":"Maria","text":"oi de novo","isClearable":true,"actions":["Responder"]}}'
wait_for 5 has "$tmp/notes.log" "NOTIFY $nid | Chat | Maria: oi de novo |.*replaces=$nid" || fail "an update did not replace the notification"
send '{"type":"kdeconnect.notification","body":{"id":"quiet","appName":"Chat","title":"x","text":"preexisting","silent":true}}'
sleep 0.5
has "$tmp/notes.log" preexisting && fail "a silent notification was shown"
echo "action $nid a0" >>"$tmp/notes.cmd"
wait_for 5 phone_has '"type":"kdeconnect.notification.action"' || fail "action was not sent"
phone_has '"key":"n1"' && phone_has '"action":"Responder"' || fail "action fields"
echo "dismiss $nid" >>"$tmp/notes.cmd"
wait_for 5 phone_has '"cancel":"n1"' || fail "dismissing was not sent to the phone"
send '{"type":"kdeconnect.notification","body":{"id":"n2","appName":"Mail","title":"Mail","text":"novo e-mail","isClearable":false}}'
wait_for 5 has "$tmp/notes.log" "| Mail | novo e-mail |" || fail "plain notification (title equals app)"
nid2=$(sed -n 's/^NOTIFY \([0-9]*\) | Mail | novo e-mail.*/\1/p' "$tmp/notes.log" | head -1)
send '{"type":"kdeconnect.notification","body":{"id":"org.kde.kdeconnect_tp::n2","isCancel":true}}'
wait_for 5 has "$tmp/notes.log" "CLOSE $nid2" || fail "a cancel from the phone did not close the notification"
echo "notifications ok"

# ---- commands -----------------------------------------------------------------------------------------
send '{"type":"kdeconnect.runcommand.request","body":{"requestCommandList":true}}'
wait_for 5 phone_has '"commandList":"{\\"Say hi\\"' || fail "command list"
phone_has '"canAddCommand":false' || fail "canAddCommand"
send '{"type":"kdeconnect.runcommand.request","body":{"key":"Say hi"}}'
wait_for 5 phone_has '"commandFinished":true,"success":true,"exitCode":0' || fail "Say hi did not finish"
phone_has '"commandStarted":true,"command":"echo hello; echo world"' || fail "commandStarted"
phone_has '"stdout":\["hello","world"\]' || fail "command output"
send '{"type":"kdeconnect.runcommand.request","body":{"key":"Fail"}}'
wait_for 5 phone_has '"success":false,"exitCode":3' || fail "a failing command"
phone_has '"stdout":\["oops"\]' || fail "stderr should join the output"
n=$(count "$tmp/phone.out" commandStarted)
send '{"type":"kdeconnect.runcommand.request","body":{"key":"echo pwned"}}'
send '{"type":"kdeconnect.runcommand.request","body":{"key":"Nope"}}'
sleep 0.5
[ "$(count "$tmp/phone.out" commandStarted)" -eq "$n" ] || fail "a command that is not configured was started"
send '{"type":"kdeconnect.runcommand.request","body":{"key":"Sleeper"}}'
wait_for 5 sh -c "[ \$(grep -c commandStarted '$tmp/phone.out') -eq $((n + 1)) ]" || fail "Sleeper did not start"
send '{"type":"kdeconnect.runcommand.request","body":{"stop":true}}'
wait_for 5 phone_has '"success":false,"exitCode":143' || fail "stop did not end Sleeper"
echo "commands ok"

# ---- mouse and keyboard ---------------------------------------------------------------------------------
"$xtool" events >>"$tmp/x.events" 2>&1 &
pids="$pids $!"
sleep 0.5
set -- $("$xtool" pointer); x0=$2; y0=$3
send '{"type":"kdeconnect.mousepad.request","body":{"dx":10.6,"dy":-4.2}}'
send '{"type":"kdeconnect.mousepad.request","body":{"dx":0.5,"dy":0.5}}'
wait_for 5 sh -c "set -- \$('$xtool' pointer); [ \$2 -eq $((x0 + 11)) ] && [ \$3 -eq $((y0 - 4)) ]" ||
    fail "pointer move (fractions carry over): was $x0,$y0, now $("$xtool" pointer)"
send '{"type":"kdeconnect.mousepad.request","body":{"x":100,"y":50}}'
wait_for 5 sh -c "[ \"\$('$xtool' pointer)\" = 'pointer 100 50' ]" || fail "absolute move"
send '{"type":"kdeconnect.mousepad.request","body":{"singleclick":true}}'
wait_for 5 has "$tmp/x.events" "ButtonRelease 1" || fail "single click"
send '{"type":"kdeconnect.mousepad.request","body":{"rightclick":true}}'
wait_for 5 has "$tmp/x.events" "ButtonPress 3" || fail "right click"
send '{"type":"kdeconnect.mousepad.request","body":{"middleclick":true}}'
wait_for 5 has "$tmp/x.events" "ButtonPress 2" || fail "middle click"
send '{"type":"kdeconnect.mousepad.request","body":{"scroll":true,"dy":1.0}}'
wait_for 5 has "$tmp/x.events" "ButtonPress 4" || fail "scroll up"
send '{"type":"kdeconnect.mousepad.request","body":{"scroll":true,"dy":-1.0}}'
wait_for 5 has "$tmp/x.events" "ButtonPress 5" || fail "scroll down"
send '{"type":"kdeconnect.mousepad.request","body":{"doubleclick":true}}'
wait_for 5 sh -c "[ \$(grep -ac 'ButtonPress 1' '$tmp/x.events') -ge 3 ]" || fail "double click"
: >"$tmp/x.events"
send '{"type":"kdeconnect.mousepad.request","body":{"key":"a"}}'
wait_for 5 has "$tmp/x.events" "KeyPress sym=0x61" || fail "typing a"
send '{"type":"kdeconnect.mousepad.request","body":{"specialKey":12}}'
wait_for 5 has "$tmp/x.events" "KeyPress sym=0xff0d" || fail "Return"
send '{"type":"kdeconnect.mousepad.request","body":{"key":"c","ctrl":true}}'
wait_for 5 has "$tmp/x.events" "KeyPress sym=0x63" || fail "ctrl+c"
ctrl=$(grep -an 'KeyPress sym=0xffe3' "$tmp/x.events" | tail -1 | cut -d: -f1)
cee=$(grep -an 'KeyPress sym=0x63' "$tmp/x.events" | tail -1 | cut -d: -f1)
ctrl_up=$(grep -an 'KeyRelease sym=0xffe3' "$tmp/x.events" | tail -1 | cut -d: -f1)
[ -n "$ctrl" ] && [ "$ctrl" -lt "$cee" ] && [ "$cee" -lt "$ctrl_up" ] || fail "ctrl is not held around the key"
: >"$tmp/x.events"
# characters the layout lacks: lent a keycode, typed in order
send '{"type":"kdeconnect.mousepad.request","body":{"key":"aç😀b"}}'
wait_for 5 has "$tmp/x.events" "KeyPress sym=0x62" || fail "typing text with accents and emoji: $(cat "$tmp/x.events")"
seq=$(grep -a KeyPress "$tmp/x.events" | sed 's/.*sym=//' | tr '\n' ' ')
[ "$seq" = "0x61 0xe7 0x101f600 0x62 " ] || fail "typed in the wrong order or wrong symbols: $seq"
echo "input ok"

# ---- the presentation pointer ----------------------------------------------------------------------------
send '{"type":"kdeconnect.presenter","body":{"dx":0.1,"dy":0.0}}'
wait_for 5 sh -c "'$xtool' windows | grep -q '^win '" || fail "no presenter window"
line=$("$xtool" windows | head -1)
echo "$line" | grep -q 'inputrects=0' || fail "the pointer window takes input: $line"
size=$(echo "$line" | sed 's/win \([0-9]*\)x.*/\1/')
posx=$(echo "$line" | sed 's/.*+\([0-9]*\)+[0-9]*.*/\1/')
[ "$size" -ge 12 ] && [ "$((posx + size / 2))" -eq 480 ] || fail "pointer position (800 wide, 0.6): $line"
sleep 0.9
[ -z "$("$xtool" windows)" ] || fail "the pointer did not go away by itself"
send '{"type":"kdeconnect.presenter","body":{"dx":0.0,"dy":0.0}}'
wait_for 5 sh -c "'$xtool' windows | grep -q '^win '" || fail "no presenter window (2)"
send '{"type":"kdeconnect.presenter","body":{"stop":true}}'
wait_for 5 sh -c "[ -z \"\$('$xtool' windows)\" ]" || fail "stop did not hide the pointer"
echo "presenter ok"

# ---- the pointer's look ([pointer] in the ini; changes apply the next time it appears) ----------
cli --set pointer.shape=square && cli --set pointer.size=huge && cli --set pointer.hide_after=0 || fail "--set pointer"
send '{"type":"kdeconnect.presenter","body":{"dx":0.0,"dy":0.0}}'
wait_for 5 sh -c "'$xtool' windows | grep -q '^win 13x13+'" || fail "the pointer did not take the new size: $("$xtool" windows)"
send '{"type":"kdeconnect.presenter","body":{"stop":true}}'
wait_for 5 sh -c "[ -z \"\$('$xtool' windows)\" ]" || fail "stop (shape)"
# an image replaces the shape, scaled to fit the size and keeping its proportions (40x20 → 12x6)
python3 - "$tmp/dot.png" <<'PY'
import struct, sys, zlib
w, h = 40, 20
raw = b"".join(b"\x00" + b"\xff\x00\x00\xff" * w for _ in range(h))
def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
PY
cli --set pointer.size= && cli --set "pointer.image=$tmp/dot.png" || fail "--set image"
send '{"type":"kdeconnect.presenter","body":{"dx":0.0,"dy":0.0}}'
wait_for 5 sh -c "'$xtool' windows | grep -q '^win 12x6+'" || fail "the image pointer: $("$xtool" windows)"
sleep 0.9
[ -n "$("$xtool" windows)" ] || fail "hide_after=0 still hid the pointer"
send '{"type":"kdeconnect.presenter","body":{"stop":true}}'
wait_for 5 sh -c "[ -z \"\$('$xtool' windows)\" ]" || fail "stop (image)"
# an animated GIF (two frames, 100 and 200 ms): the frames keep changing while it is up
printf 'GIF89a\002\000\001\000\200\000\000\377\000\000\000\000\377!\371\004\005\012\000\001\000,\000\000\000\000\002\000\001\000\000\002\002D\012\000!\371\004\004\024\000\000\000,\000\000\000\000\002\000\001\000\000\002\002\014\012\000;' >"$tmp/dot.gif"
cli --set "pointer.image=$tmp/dot.gif" || fail "--set gif"
send '{"type":"kdeconnect.presenter","body":{"dx":0.0,"dy":0.0}}'
wait_for 5 sh -c "'$xtool' windows | grep -q '^win 12x6+'" || fail "the gif pointer: $("$xtool" windows)"
sleep 1
[ -n "$("$xtool" windows)" ] || fail "the animated pointer went away"
send '{"type":"kdeconnect.presenter","body":{"stop":true}}'
wait_for 5 sh -c "[ -z \"\$('$xtool' windows)\" ]" || fail "stop (gif)"
# a broken image falls back to the shape instead of showing nothing
printf 'not an image' >"$tmp/dot.png"
cli --set "pointer.image=$tmp/dot.png"
send '{"type":"kdeconnect.presenter","body":{"dx":0.0,"dy":0.0}}'
wait_for 5 sh -c "'$xtool' windows | grep -q '^win 12x12+'" || fail "a bad image left no pointer: $("$xtool" windows)"
send '{"type":"kdeconnect.presenter","body":{"stop":true}}'
cli --set pointer.shape= && cli --set pointer.image= && cli --set pointer.hide_after=
echo "pointer look ok"

# ---- keeping the screen awake -----------------------------------------------------------------------------------
wait_for 5 has "$tmp/ss.log" "INHIBIT Kiremote | Phone is connected | 42" || fail "the screensaver was not inhibited"
send '{"type":"kdeconnect.ping","body":{}}'
# lock: the state at connection, then changes
phone_has '"requestLocked":null' || fail "we did not ask for the phone's lock state"
send '{"type":"kdeconnect.lock.request","body":{"requestLocked":null}}'
wait_for 5 phone_has '"isLocked":false' || fail "lock state reply"
echo "locked true" >>"$tmp/login.cmd"
wait_for 5 phone_has '"isLocked":true' || fail "a logind change was not sent"
n=$(count "$tmp/phone.out" '"isLocked"')
echo "other true" >>"$tmp/login.cmd"; echo "locked false" >>"$tmp/login.cmd"
wait_for 5 sh -c "[ \$(grep -c '\"isLocked\":false' '$tmp/phone.out') -ge 2 ]" || fail "unlocking was not reported"
[ "$(count "$tmp/phone.out" '"isLocked"')" -eq $((n + 1)) ] || fail "another session's change was reported"
send '{"type":"kdeconnect.lock.request","body":{"setLocked":true}}'
wait_for 5 phone_has '"lockResult":true' || fail "lock request"
has "$tmp/lock.ran" lock || fail "the lock command did not run"
send '{"type":"kdeconnect.lock.request","body":{"setLocked":false}}'
sleep 0.5
has "$tmp/lock.ran" unlock && fail "unlocking from a phone is off by default"
has "$tmp/a.log" "asked to unlock this computer; refused" || fail "no log of the refused unlock"
echo "lock ok"

# ---- the screensaver when the phone goes, and without a service -----------------------------------------------------
kill "$phone"
wait_for 5 has "$tmp/ss.log" "UNINHIBIT 42" || fail "the screensaver was not released"
has "$tmp/ss.log" ACTIVITY || fail "no SimulateUserActivity after releasing"
kill "$sspid"
sleep 0.3
start_phone
wait_for 5 sh -c "grep -q '^suspend 0x' '$tmp/xdgss'" || fail "no fallback to xdg-screensaver"
win=$(sed -n 's/^suspend //p' "$tmp/xdgss" | head -1)
kill "$phone"
wait_for 5 has "$tmp/xdgss" "resume $win" || fail "xdg-screensaver was not resumed"
echo "screensaver ok"

# ---- clean exit -------------------------------------------------------------------------------------------------------
exec 7>&-
kill "$daemon"
wait "$daemon" 2>/dev/null || true
pids=$(echo "$pids" | sed "s/ $daemon//")
if grep -q "Sanitizer\|runtime error" "$tmp/a.log"; then fail "sanitizer report"; fi

echo "tests/net/desktop.sh: all passed"
