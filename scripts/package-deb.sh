#!/bin/sh
# Build an unprivileged package artifact; does not install or publish it.
set -eu
cd "$(dirname "$0")/.."
version=${VERSION:?Set a numeric VERSION, e.g. 0.3.0}
arch=${1:?usage: package-deb.sh amd64|arm64}
case "$version" in ''|*[!0-9.]*) echo 'Numeric version required' >&2; exit 2;; esac
case "$arch" in amd64|arm64) ;; *) exit 2;; esac
for binary in geist geist-app geistd; do
    test -x "$binary"
    file "$binary" | grep -Eq 'statically linked|static-pie linked' || { echo "$binary must be a static Linux executable" >&2; exit 1; }
    case "$arch" in amd64) file "$binary" | grep -q 'x86-64';; arm64) file "$binary" | grep -q 'aarch64';; esac
done
mkdir -p build
stage=$(mktemp -d build/deb-stage.XXXXXX)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/DEBIAN" "$stage/usr/lib/geist" "$stage/usr/bin" \
    "$stage/usr/lib/systemd/user" "$stage/usr/share/applications" "$stage/usr/share/doc/geist"
install -m 755 geist geist-app geistd "$stage/usr/lib/geist/"
ln -s ../lib/geist/geist "$stage/usr/bin/geist"
install -m 644 deploy/systemd/geist.service "$stage/usr/lib/systemd/user/"
install -m 644 deploy/desktop/geist.desktop "$stage/usr/share/applications/"
install -m 644 LICENSE "$stage/usr/share/doc/geist/copyright"
install -m 644 docs/INSTALL.md "$stage/usr/share/doc/geist/README.md"
cat > "$stage/DEBIAN/control" <<EOF
Package: geist
Version: $version
Architecture: $arch
Maintainer: Geisten <geisten@users.noreply.github.com>
Section: utils
Priority: optional
Depends: ca-certificates
Recommends: systemd, xdg-utils
Description: Local model manager and shared geistd service
 Choose a model once and use it from the browser, terminal and compatible editors.
EOF
cat > "$stage/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -eu
# Never impersonate desktop users or start an inference workload as root.
echo 'Open Geist from the application menu, or run: geist open'
echo 'Enable login startup with: systemctl --user enable geist.service'
echo 'After an upgrade, run: systemctl --user daemon-reload && geist restart'
EOF
chmod 755 "$stage/DEBIAN/postinst"
cat > "$stage/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -eu
case "${1:-}" in remove|deconfigure)
    # Stop only this package's user unit through each active user's manager.
    # Never kill by process name or inspect another user's credential files.
    if command -v runuser >/dev/null && command -v systemctl >/dev/null; then
        for runtime in /run/user/[0-9]*; do
            test -S "$runtime/bus" || continue
            uid=${runtime##*/}
            case "$uid" in ''|*[!0-9]*) continue;; esac
            account=$(getent passwd "$uid" | cut -d: -f1)
            test -n "$account" || continue
            runuser -u "$account" -- env XDG_RUNTIME_DIR="$runtime" DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
                systemctl --user disable --now geist.service
        done
    fi
    ;;
esac
EOF
chmod 755 "$stage/DEBIAN/prerm"
(cd "$stage" && find usr -type f -exec md5sum {} + > DEBIAN/md5sums)
dpkg-deb --root-owner-group --build "$stage" "build/geist_${version}_${arch}.deb"
(cd build && sha256sum "geist_${version}_${arch}.deb" > "geist_${version}_${arch}.deb.sha256")
