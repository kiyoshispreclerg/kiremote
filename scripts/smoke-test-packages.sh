#!/bin/sh
# smoke-test-packages.sh - what an installed kiremote, kiremote-gtk,
# kiremote-kidesktop and kiremote-mirror must do, in a clean image where only their Depends are
# installed (so a missing one shows up here): the programs start, the daemon
# answers on its socket, and the KiDesktop modules are loaded by the real
# xisserve. Needs xvfb, x11-utils and procps besides the .debs.
set -eu

fail() { echo "smoke-test-packages: FAIL: $*" >&2; exit 1; }

tmp=$(mktemp -d)
pids=""
cleanup() { for p in $pids; do kill "$p" 2>/dev/null || true; done; rm -rf "$tmp"; }
trap cleanup EXIT

# What was installed decides what is checked: the daemon alone, with the GTK
# window, with the KiDesktop modules, with the mirror, or all.
have_gtk=no
have_kd=no
have_mirror=no
[ -x /usr/bin/kiremote-mirror ] && have_mirror=yes
[ -x /usr/bin/kiremote ] && have_gtk=yes
[ -e /usr/lib/xisserve/pages/kiremote.so ] && have_kd=yes

for b in kiremoted kiremote-cli; do
	"$b" --version >/dev/null || fail "$b --version"
done
[ "$have_gtk" = no ] || kiremote --version >/dev/null || fail "kiremote --version"
if [ "$have_kd" = yes ]; then
	for f in /usr/lib/kiremote/kiremote-tab.so /usr/lib/xisserve/pages/kiremote.so /usr/lib/xispanel/widgets/kiremote.so; do
		[ -e "$f" ] || fail "$f is not installed"
		# Only what the host program provides (GTK, xpw_*, xisserve_*) may be missing.
		missing=$(ldd "$f" | grep 'not found' || true)
		[ -z "$missing" ] || fail "$f: $missing"
	done
	[ -e /usr/share/kiconf/tabs/kiremote.desktop ] || fail "no kiconf tab"
	grep -q '^X-KiConf-Module=/usr/lib/kiremote/kiremote-tab.so$' /usr/share/kiconf/tabs/kiremote.desktop || fail "the tab does not point at its module"
fi

export HOME=$tmp XDG_CONFIG_HOME=$tmp/cfg XDG_CACHE_HOME=$tmp/cache XDG_DATA_HOME=$tmp/data XDG_RUNTIME_DIR=$tmp/run \
	LC_ALL=C.UTF-8 NO_AT_BRIDGE=1
unset DBUS_SESSION_BUS_ADDRESS
mkdir -p "$tmp/run" "$tmp/cfg" "$tmp/cache" "$tmp/data"
chmod 700 "$tmp/run"

# -v: the debug lines are how the test sees who connected. Only ever kill what
# this script started (there may be a daemon of a real session on the machine).
kiremoted -v -c "$tmp/kiremote.ini" >"$tmp/daemon.log" 2>&1 &
pids="$pids $!"
t=50
while [ ! -S "$tmp/run/kiremote.sock" ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
[ -S "$tmp/run/kiremote.sock" ] || { cat "$tmp/daemon.log"; fail "the daemon did not start"; }
kiremote-cli --status | grep -q 'kiremoted' || fail "--status"
kiremote-cli --schema | grep -q 'plugins.ping' || fail "--schema"
kiremote-cli --set clipboard.auto_share=off >/dev/null || fail "--set"
[ "$(kiremote-cli --get clipboard.auto_share)" = false ] || fail "--get after --set"
[ -s "$tmp/run/kiremote-status.json" ] || fail "no status file"

# The mirror: it starts, finds its libraries and the scrcpy-server it copies to
# the phone, and with an adb that sees no phone says so (no X, no phone here).
if [ "$have_mirror" = yes ]; then
	missing=$(ldd /usr/bin/kiremote-mirror | grep 'not found' || true)
	[ -z "$missing" ] || fail "kiremote-mirror: $missing"
	kiremote-mirror --version | grep -q 'scrcpy-server' || fail "kiremote-mirror --version"
	kiremote-mirror --help | grep -q -- '--wifi' || fail "kiremote-mirror --help"
	[ -s /usr/share/kiremote/scrcpy-server ] || fail "no scrcpy-server in /usr/share/kiremote"
	printf '#!/bin/sh\necho "List of devices attached"\n' > "$tmp/adb"
	chmod +x "$tmp/adb"
	out=$(KIREMOTE_ADB="$tmp/adb" kiremote-mirror -c "$tmp/kiremote.ini" 2>&1) && fail "kiremote-mirror without a phone succeeded"
	echo "$out" | grep -q 'No phone found' || { echo "$out"; fail "kiremote-mirror without a phone: $out"; }
	[ "$(kiremote-cli --schema | grep -c '^ *mirror\.')" -ge 1 ] || fail "the daemon does not know the [mirror] settings"
fi

if [ "$have_gtk" = no ] && [ "$have_kd" = no ]; then
	[ "$have_mirror" = no ] && echo "smoke-test-packages: ok (daemon only)" || echo "smoke-test-packages: ok (daemon and mirror)"
	exit 0
fi

# The window and the page, in a private X server.
n=40
while [ -e "/tmp/.X11-unix/X$n" ]; do n=$((n + 1)); done
Xvfb ":$n" -screen 0 800x600x24 -nolisten tcp >"$tmp/xvfb.log" 2>&1 &
pids="$pids $!"
t=50
while [ ! -e "/tmp/.X11-unix/X$n" ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
export DISPLAY=":$n"

subscribers() { grep -c 'control client subscribed' "$tmp/daemon.log" || true; }

if [ "$have_gtk" = yes ]; then
	kiremote --hidden >"$tmp/gtk.log" 2>&1 &
	gtk=$!
	pids="$pids $gtk"
	t=100
	while [ "$(subscribers)" -lt 1 ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
	[ "$(subscribers)" -ge 1 ] || { cat "$tmp/gtk.log"; fail "kiremote did not reach the daemon"; }
	kill -0 "$gtk" 2>/dev/null || fail "kiremote exited"
	timeout 5 kiremote --settings || fail "a second kiremote did not hand over and exit"
fi

if [ "$have_kd" = yes ] && command -v xisserve >/dev/null 2>&1; then
	before=$(subscribers)
	xisserve --kiremote >"$tmp/xisserve.log" 2>&1 &
	pids="$pids $!"
	t=150
	while [ "$(subscribers)" -le "$before" ] && [ $t -gt 0 ]; do sleep 0.1; t=$((t - 1)); done
	[ "$(subscribers)" -gt "$before" ] || { cat "$tmp/xisserve.log"; fail "the xisserve page was not loaded"; }
	grep -q 'not loaded' "$tmp/xisserve.log" && fail "xisserve: $(cat "$tmp/xisserve.log")"
fi
echo "smoke-test-packages: ok"
