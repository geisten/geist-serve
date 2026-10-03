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
# BIN_DIR: where the three executables are (release CI: the static musl build).
bin=${BIN_DIR:-.}
cp "$bin/geist" "$bin/geist-app" "$bin/geistd" "$destination/"
cp scripts/start-geist.sh "$destination/Start Geisten.sh"
chmod 755 "$destination/Start Geisten.sh" "$destination/geist" "$destination/geist-app" "$destination/geistd"
cp web/vendor/marked-LICENSE "$destination/marked-LICENSE"
cp web/vendor/katex-LICENSE "$destination/katex-LICENSE"
cp LICENSE "$destination/LICENSE"
python3 scripts/engine-provenance.py package "$bin/geistd" --require-clean --output "$destination/ENGINE.json"
cp docs/APP.md "$destination/README.md"
if [ -f build/app-build-packages.txt ]; then cp build/app-build-packages.txt "$destination/BUILD-PACKAGES.txt"; fi
if [ "${platform#linux}" != "$platform" ]; then
    for b in geist geist-app; do file "$bin/$b" | grep -Eq 'statically linked|static-pie linked' || { echo "Linux $b must be static" >&2; exit 1; }; done
    file "$bin/geistd" | grep -Eq 'statically linked|static-pie linked' || { echo 'Linux daemon must be static' >&2; exit 1; }
fi
(cd "$destination" && if command -v sha256sum >/dev/null; then sha256sum geist geist-app geistd > SHA256SUMS; else shasum -a 256 geist geist-app geistd > SHA256SUMS; fi)
tar -czf "$destination.tar.gz" -C build "$(basename "$destination")"
printf '%s\n' "$destination.tar.gz"
