#!/bin/sh
# fetch-scrcpy-server.sh - downloads the scrcpy-server that kiremote-mirror
# pushes to the phone, the version and checksum config.mk pins, and checks it.
# Building the jar needs the Android SDK, so it is not part of our build.
#
#   scripts/fetch-scrcpy-server.sh [OUT]   default build/scrcpy-server
#   SCRCPY_SERVER=/path/scrcpy-server       check and copy this file instead
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
version=$(sed -n 's/^SCRCPY_VERSION *= *//p' "$here/config.mk")
sha=$(sed -n 's/^SCRCPY_SERVER_SHA256 *= *//p' "$here/config.mk")
out=${1:-$here/build/scrcpy-server}
mkdir -p "$(dirname "$out")"
tmp=$out.part
if [ -n "${SCRCPY_SERVER:-}" ]; then
	cp "$SCRCPY_SERVER" "$tmp"
else
	url=https://github.com/Genymobile/scrcpy/releases/download/v$version/scrcpy-server-v$version
	echo "fetch-scrcpy-server: $url"
	curl -fsSL -o "$tmp" "$url"
fi
if ! echo "$sha  $tmp" | sha256sum --check --status; then
	rm -f "$tmp"
	echo "fetch-scrcpy-server: checksum mismatch (expected $sha for $version)" >&2
	exit 1
fi
mv "$tmp" "$out"
echo "fetch-scrcpy-server: $out ($version)"
