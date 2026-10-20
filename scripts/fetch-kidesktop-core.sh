#!/bin/sh
# fetch-kidesktop-core.sh - installs kidesktop-core for this distro from the
# newest kidesktop GitHub release that has one (its assets are named
# kidesktop-core_<version>~<distro><release>_<arch>.deb, see kidesktop's
# scripts/build-deb.sh). Kiremote needs it to build the KiDesktop modules (the
# headers) and the smoke tests need it to run them.
#
#   KIDESKTOP_CORE_DEB=/path/kidesktop-core.deb   use this file instead
#   KIDESKTOP_REPO=owner/name                      default kiyoshispreclerg/kidesktop
#   GITHUB_TOKEN                                   raises the API rate limit
set -eu

SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive

if [ -n "${KIDESKTOP_CORE_DEB:-}" ]; then
	deb=$KIDESKTOP_CORE_DEB
else
	. /etc/os-release
	arch=$(dpkg --print-architecture)
	repo=${KIDESKTOP_REPO:-kiyoshispreclerg/kidesktop}
	pattern="kidesktop-core_[^/\"]*~${ID}${VERSION_ID:-}_${arch}\\.deb"
	command -v curl >/dev/null 2>&1 || $SUDO apt-get install -y --no-install-recommends curl ca-certificates
	auth=""
	[ -z "${GITHUB_TOKEN:-}" ] || auth="Authorization: Bearer $GITHUB_TOKEN"
	# Newest first; pre-releases count (kidesktop is 0.x).
	url=$(curl -fsSL ${auth:+-H "$auth"} "https://api.github.com/repos/$repo/releases?per_page=20" |
		grep -o "\"browser_download_url\": *\"[^\"]*${pattern}\"" | head -n1 | sed 's/.*"\(https[^"]*\)"$/\1/')
	[ -n "$url" ] || { echo "fetch-kidesktop-core: no kidesktop-core for ${ID}${VERSION_ID:-} ${arch} in $repo's releases" >&2; exit 1; }
	deb=$(mktemp -d)/$(basename "$url")
	echo "fetch-kidesktop-core: $url"
	curl -fsSL -o "$deb" "$url"
fi
# apt, not dpkg -i: the package's own dependencies come along.
$SUDO apt-get install -y --no-install-recommends "$deb"
