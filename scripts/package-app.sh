#!/bin/sh
# Assemble a portable app pair; no models are bundled and no service is installed.
set -eu
cd "$(dirname "$0")/.."
platform=${1:?usage: package-app.sh linux-aarch64|linux-x86_64|macos-arm64}
case "$platform" in linux-aarch64|linux-x86_64|macos-arm64) ;; *) exit 2;; esac
version=${VERSION:-dev}
case "$version" in *[!A-Za-z0-9._-]*) exit 2;; esac
destination="build/geist-$version-$platform"
mkdir -p "$destination"
cp geist geist-app geistd "$destination/"
cp scripts/start-geist.sh "$destination/Start Geist.sh"
chmod 755 "$destination/Start Geist.sh" "$destination/geist" "$destination/geist-app" "$destination/geistd"
cp LICENSE "$destination/LICENSE"
cp docs/APP.md "$destination/README.md"
if [ -f build/app-build-packages.txt ]; then cp build/app-build-packages.txt "$destination/BUILD-PACKAGES.txt"; fi
if [ "${platform#linux}" != "$platform" ]; then
    file geist-app | grep -Eq 'statically linked|static-pie linked' || { echo 'Linux app must be static' >&2; exit 1; }
    file geistd | grep -Eq 'statically linked|static-pie linked' || { echo 'Linux daemon must be static' >&2; exit 1; }
fi
(cd "$destination" && if command -v sha256sum >/dev/null; then sha256sum geist geist-app geistd > SHA256SUMS; else shasum -a 256 geist geist-app geistd > SHA256SUMS; fi)
tar -czf "$destination.tar.gz" -C build "$(basename "$destination")"
printf '%s\n' "$destination.tar.gz"
