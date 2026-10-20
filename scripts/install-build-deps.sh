#!/bin/sh
# install-build-deps.sh - apt-installs debian/control's Build-Depends (the one
# list of what Kiremote needs to build), plus any extra packages given as
# arguments. Used by CI; also works on a dev machine (as root, or via sudo).
#
# kidesktop-core is not in the distributions: it comes from kidesktop's GitHub
# releases (scripts/fetch-kidesktop-core.sh), unless it is already installed or
# NO_KIDESKTOP_CORE=1 (jobs that only lint and test the daemon).
#
#   sudo ./scripts/install-build-deps.sh cppcheck xvfb
set -eu
cd "$(dirname "$0")/.."

deps=$(sed -n '/^Build-Depends:/,/^[A-Za-z-]*:/p' debian/control |
	sed -e '1s/^Build-Depends://' -e '/^[A-Za-z-]*:/d' -e '/^#/d' |
	tr ',' '\n' | sed -e 's/([^)]*)//g' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' |
	sed -e 's/^debhelper-compat$/debhelper/' | grep -v '^$' | grep -vx 'kidesktop-core')

SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update
# shellcheck disable=SC2086 # word splitting is the point
$SUDO apt-get install -y --no-install-recommends build-essential dpkg-dev git ca-certificates $deps "$@"

if [ -z "${NO_KIDESKTOP_CORE:-}" ] && ! dpkg-query -W -f='${Status}' kidesktop-core 2>/dev/null | grep -q 'ok installed'; then
	./scripts/fetch-kidesktop-core.sh
fi
