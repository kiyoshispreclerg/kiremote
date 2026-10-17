#!/bin/sh
# Makes the pairing verification key test vector with the openssl CLI
# only, independently of src/cert.c (PLANO.md, phase 1 acceptance):
#
#   tests/fixtures/pair-a.pem, pair-b.pem   two self-signed certificates
#   tests/fixtures/pair-key.txt             "<timestamp> <key>"
#
# The key is upstream's PairingHandler::verificationKey: SHA-256 over the
# two DER public keys (larger first, byte comparison) and the timestamp in
# decimal; first 8 hex digits, upper case.
set -eu
out=${1:-tests/fixtures}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mk() { # name id algorithm-options
    openssl req -x509 -new -nodes -days 3650 -sha512 $3 \
        -subj "/CN=$2/O=KDE/OU=KDE Connect" \
        -keyout "$tmp/$1.key" -out "$out/pair-$1.pem" 2>/dev/null
    openssl x509 -in "$out/pair-$1.pem" -pubkey -noout |
        openssl pkey -pubin -outform DER -out "$tmp/$1.der"
}
mk a 0123456789abcdef0123456789abcdef "-newkey ec -pkeyopt ec_paramgen_curve:P-256"
mk b fedcba9876543210fedcba9876543210 "-newkey rsa:2048"

ts=1760000000
# Byte comparison, a shorter prefix being smaller: compare the hex dumps,
# padded so a prefix sorts first.
ha=$(od -An -tx1 -v "$tmp/a.der" | tr -d ' \n')
hb=$(od -An -tx1 -v "$tmp/b.der" | tr -d ' \n')
if [ "$(printf '%s\n%s\n' "$ha" "$hb" | LC_ALL=C sort | head -n1)" = "$ha" ]; then
    first=b second=a # a is smaller: the larger (b) goes first
else
    first=a second=b
fi
key=$( (cat "$tmp/$first.der" "$tmp/$second.der"; printf '%s' "$ts") |
    openssl dgst -sha256 -r | cut -c1-8 | tr a-f A-F)
echo "$ts $key" > "$out/pair-key.txt"
cat "$out/pair-key.txt"
