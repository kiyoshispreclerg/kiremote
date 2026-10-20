#!/bin/sh
# build-deb.sh - builds the kiremote, kiremote-gtk and kiremote-kidesktop
# .debs into dist/ for the distro it runs on (see debian/README.source).
# Needs debhelper, dpkg-dev and the Build-Depends of debian/control installed
# (scripts/install-build-deps.sh does that, kidesktop-core included).
#
# Version: KIREMOTE_VERSION if set, else the exact git tag (v1.2.3 -> 1.2.3),
# else <VERSION in config.mk>+git<commit date>.<commit>; always followed by
# ~<distro><release> (~ubuntu22.04, ~debian12), which sorts below the bare
# version, so a later official package still upgrades over it.
#
# KIREMOTE_SKIP_DEPS_CHECK=1 passes -d to dpkg-buildpackage: for a machine
# where kidesktop-core was built from source (headers in /usr/local/include,
# no package) instead of installed as a .deb.
set -eu
cd "$(dirname "$0")/.."

. /etc/os-release
suffix="~${ID}${VERSION_ID:-}"

if [ -n "${KIREMOTE_VERSION:-}" ]; then
	base="${KIREMOTE_VERSION#v}"
elif tag=$(git describe --tags --exact-match 2>/dev/null); then
	base="${tag#v}"
else
	base="$(sed -n 's/^VERSION[[:space:]]*=[[:space:]]*//p' config.mk | tr -d ' \t\r\n')"
	if git rev-parse --short HEAD >/dev/null 2>&1; then
		base="${base}+git$(git log -1 --format=%cd --date=format:%Y%m%d).$(git rev-parse --short HEAD)"
	fi
fi
version="${base}${suffix}"

cat > debian/changelog <<CHANGELOG
kiremote (${version}) unstable; urgency=medium

  * Build of $(git rev-parse --short HEAD 2>/dev/null || echo unknown).

 -- Kiyoshi Spreclerg <kiyoshi_pip@protonmail.com>  $(date -R)
CHANGELOG

# The build tree and the make variables a previous `make` left are not the
# package's (PREFIX is baked into the objects).
make clean >/dev/null 2>&1 || true

flags=""
[ -z "${KIREMOTE_SKIP_DEPS_CHECK:-}" ] || flags="-d"
# shellcheck disable=SC2086
dpkg-buildpackage -b -us -uc $flags

mkdir -p dist
arch=$(dpkg --print-architecture)
for pkg in kiremote kiremote-gtk kiremote-kidesktop; do
	mv "../${pkg}_${version}_${arch}.deb" dist/
	rm -f "../${pkg}-dbgsym_${version}_${arch}.ddeb" "../${pkg}-dbgsym_${version}_${arch}.deb"
	echo "build-deb: dist/${pkg}_${version}_${arch}.deb"
done
rm -f "../kiremote_${version}_${arch}.buildinfo" "../kiremote_${version}_${arch}.changes"
