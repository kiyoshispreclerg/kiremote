#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two kiremoted ("Alfa" and "Beta") in two network namespaces joined by a
# veth pair, never on the host's network: discovery, pairing with the same
# verification key on both sides, ping both ways, files both ways,
# reconnection after a restart without pairing again, and unpairing.
#
#   tests/net/pair.sh BUILD_DIR
#
# Needs unprivileged user namespaces (unshare -rn); skips without them.
set -eu

if [ "${1:-}" != --inside ]; then
    build=$(cd "${1:-build}" && pwd)
    if ! unshare -rn true 2>/dev/null; then
        echo "tests/net/pair.sh: skipped (no unprivileged network namespaces)"
        exit 0
    fi
    exec unshare -rn sh "$0" --inside "$build"
fi
build=$2
export LC_ALL=C LANGUAGE=en

tmp=$(mktemp -d /tmp/kiremote-net-XXXXXX)
pids=""
holder=""
cleanup() {
    for p in $pids $holder; do kill "$p" 2>/dev/null || true; done
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

# wait_for SECONDS COMMAND...: retries the command until it succeeds.
wait_for() {
    t=$(($1 * 10)); shift
    while [ "$t" -gt 0 ]; do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
        t=$((t - 1))
    done
    return 1
}

# ---- network: this namespace (Alfa) <-> the holder's (Beta) --------------
ip link set lo up
ip link add va type veth peer name vb
unshare -n sleep 1000 &
holder=$!
other_ns() { test "$(readlink /proc/$holder/ns/net)" != "$(readlink /proc/$$/ns/net)"; }
wait_for 5 other_ns || fail "no second namespace"
ip link set vb netns "$holder"
ip addr add 10.77.0.1/24 brd + dev va
ip link set va up
nsenter -t "$holder" -n sh -c \
    'ip link set lo up && ip addr add 10.77.0.2/24 brd + dev vb && ip link set vb up'

# ---- the two daemons ---------------------------------------------------------
for who in a b; do
    mkdir -p "$tmp/$who/cfg" "$tmp/$who/run" "$tmp/$who/share"
done
printf '[device]\nname = Alfa\ntype = desktop\n[share]\ndir = %s\n' "$tmp/a/share" >"$tmp/a/kiremote.ini"
printf '[device]\nname = Beta\ntype = laptop\n[share]\ndir = %s\n' "$tmp/b/share" >"$tmp/b/kiremote.ini"

start() { # who
    # A plain command (not a function), so $! is the daemon itself:
    # nsenter only forks to enter a PID namespace.
    if [ "$1" = a ]; then run=""; else run="nsenter -t $holder -n"; fi
    # Never the user's own session bus: its socket is a file, so it is
    # reachable from the namespace.
    env -u DBUS_SESSION_BUS_ADDRESS \
        XDG_CONFIG_HOME=$tmp/$1/cfg XDG_RUNTIME_DIR=$tmp/$1/run HOME=$tmp/$1 \
        $run "$build/kiremoted" -v -c "$tmp/$1/kiremote.ini" >>"$tmp/$1.log" 2>&1 &
    pids="$pids $!"
    eval "pid_$1=\$!"
}
cli() { # who args...
    who=$1; shift
    "$build/kiremote-cli" -s "$tmp/$who/run/kiremote.sock" "$@"
}
lists() { # who pattern
    cli "$1" -l | grep -q "$2"
}

start a
wait_for 5 test -S "$tmp/a/run/kiremote.sock" || fail "Alfa did not start"
start b
wait_for 5 test -S "$tmp/b/run/kiremote.sock" || fail "Beta did not start"

# Beta's broadcast reaches Alfa, which connects to it.
wait_for 10 lists a "^- Beta: .*(reachable, not paired)" || fail "Alfa does not see Beta"
wait_for 10 lists b "^- Alfa: .*(reachable, not paired)" || fail "Beta does not see Alfa"
id_a=$(cli b -l | sed -n 's/^- Alfa: \([^ ]*\) .*/\1/p')
id_b=$(cli a -l | sed -n 's/^- Beta: \([^ ]*\) .*/\1/p')
echo "Alfa is $id_a, Beta is $id_b"
grep -q "connected to Beta (laptop, 10.77.0.2)" "$tmp/a.log" || fail "Alfa's log"

# ---- pairing -----------------------------------------------------------------
cli a -d "$id_b" --pair >"$tmp/pair.out" 2>&1 &
pair_pid=$!
wait_for 10 lists b "asks to pair" || fail "Beta got no pairing request"
key_b=$(cli b -l | sed -n 's/.*verification key \([0-9A-F]\{8\}\).*/\1/p')
wait_for 5 grep -q "Verification key" "$tmp/pair.out" || fail "Alfa shows no key"
key_a=$(sed -n 's/^Verification key: //p' "$tmp/pair.out")
[ -n "$key_a" ] && [ "$key_a" = "$key_b" ] || fail "keys differ: '$key_a' vs '$key_b'"
echo "verification key $key_a on both sides"
cli b -d Alfa --pair >/dev/null || fail "Beta could not accept"
wait "$pair_pid" || fail "Alfa's --pair failed: $(cat "$tmp/pair.out")"
grep -q "Paired with Beta" "$tmp/pair.out" || fail "Alfa's --pair output"
lists a "Beta: .*(paired and reachable)" || fail "Alfa does not list Beta as paired"
lists b "Alfa: .*(paired and reachable)" || fail "Beta does not list Alfa as paired"
test -f "$tmp/a/cfg/kiremote/trusted/$id_b.pem" || fail "Alfa did not save Beta's certificate"

# ---- ping --------------------------------------------------------------------
cli a -d Beta --ping || fail "ping Alfa -> Beta"
cli b -d "$id_a" --ping-msg "oi, tudo bem?" || fail "ping Beta -> Alfa"
wait_for 5 grep -q "Alfa: Ping!" "$tmp/b.log" || fail "Beta got no ping"
wait_for 5 grep -q "Beta: oi, tudo bem?" "$tmp/a.log" || fail "Alfa got no ping"

# ---- files -------------------------------------------------------------------
head -c 3000000 /dev/urandom >"$tmp/big file.bin"
printf 'olá\n' >"$tmp/note.txt"
: >"$tmp/empty"
cli a -d Beta --share "$tmp/big file.bin" --share "$tmp/note.txt" || fail "share Alfa -> Beta"
cli b -d Alfa --share "$tmp/note.txt" --share "$tmp/empty" || fail "share Beta -> Alfa"
wait_for 15 cmp -s "$tmp/big file.bin" "$tmp/b/share/big file.bin" || fail "big file"
wait_for 5 cmp -s "$tmp/note.txt" "$tmp/b/share/note.txt" || fail "note to Beta"
wait_for 5 cmp -s "$tmp/note.txt" "$tmp/a/share/note.txt" || fail "note to Alfa"
wait_for 5 test -f "$tmp/a/share/empty" || fail "empty file"
# The same name again gets " (1)".
cli a -d Beta --share "$tmp/note.txt" || fail "share again"
wait_for 5 cmp -s "$tmp/note.txt" "$tmp/b/share/note (1).txt" || fail "unique name"
ls "$tmp"/b/share | grep -q '\.part$' && fail "a .part file was left behind"
echo "files went both ways"

# ---- reconnection ------------------------------------------------------------
kill "$pid_b"
wait "$pid_b" 2>/dev/null || true
wait_for 10 lists a "Beta: .*(paired, not reachable)" || fail "Alfa did not notice Beta leaving"
start b
wait_for 10 lists a "Beta: .*(paired and reachable)" || fail "no reconnection"
wait_for 5 lists b "Alfa: .*(paired and reachable)" || fail "Beta forgot the pairing"
cli b -d Alfa --ping-msg "voltei" || fail "ping after reconnecting"
wait_for 5 grep -q "Beta: voltei" "$tmp/a.log" || fail "no ping after reconnecting"
echo "reconnected without pairing again"

# ---- unpairing ---------------------------------------------------------------
cli a -d Beta --unpair || fail "unpair"
wait_for 5 lists b "Alfa: .*(reachable, not paired)" || fail "Beta still paired"
lists a "Beta: .*(reachable, not paired)" || fail "Alfa still paired"
test ! -e "$tmp/a/cfg/kiremote/trusted/$id_b.pem" || fail "Alfa kept Beta's certificate"
test ! -e "$tmp/b/cfg/kiremote/trusted/$id_a.pem" || fail "Beta kept Alfa's certificate"
cli a -d Beta --ping 2>/dev/null && fail "ping to an unpaired device went through"

# ---- clean exit ----------------------------------------------------------------
kill "$pid_a" "$pid_b"
wait "$pid_a" "$pid_b" 2>/dev/null || true
pids=""
if grep -q "Sanitizer" "$tmp/a.log" "$tmp/b.log"; then fail "sanitizer report"; fi
grep -q "unpaired from Alfa" "$tmp/b.log" || fail "Beta's log"

echo "tests/net/pair.sh: all passed"
