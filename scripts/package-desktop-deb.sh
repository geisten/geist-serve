#!/bin/sh
# Optional GUI package; the daemon package has no graphical dependencies.
set -eu
cd "$(dirname "$0")/.."
version=${VERSION:?Set VERSION to X.Y.Z}
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || exit 2
mkdir -p build
stage=$(mktemp -d build/desktop-stage.XXXXXX)
chmod 755 "$stage"
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/DEBIAN" "$stage/usr/bin" "$stage/usr/lib/geisten-desktop" "$stage/usr/share/applications" "$stage/usr/share/doc/geisten-desktop" "$stage/etc/apparmor.d"
install -m 644 deploy/apparmor/geisten-desktop "$stage/etc/apparmor.d/"
printf '%s\n' /etc/apparmor.d/geisten-desktop > "$stage/DEBIAN/conffiles"
install -m 644 desktop/geist_desktop.py "$stage/usr/lib/geisten-desktop/"
printf '%s\n' '#!/bin/sh' 'exec /usr/bin/python3 /usr/lib/geisten-desktop/geist_desktop.py "$@"' > "$stage/usr/bin/geisten-desktop"
chmod 755 "$stage/usr/bin/geisten-desktop"
install -m 644 deploy/desktop/geisten.desktop "$stage/usr/share/applications/"
install -m 644 LICENSE "$stage/usr/share/doc/geisten-desktop/copyright"
cat > "$stage/DEBIAN/control" <<EOF
Package: geisten-desktop
Version: $version
Architecture: all
Maintainer: geisten <geisten@users.noreply.github.com>
Section: utils
Priority: optional
Depends: geisten (= $version), apparmor (>= 4.0), python3, python3-gi, gir1.2-gtk-4.0, gir1.2-webkit-6.0
Provides: geist-desktop
Conflicts: geist-desktop
Replaces: geist-desktop
Description: Desktop model manager for the shared geisten service
 Choose models, try tasks and configure editors in a private local app window.
EOF
cat > "$stage/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -eu
if [ "$1" = configure ] && [ -d /sys/kernel/security/apparmor ]; then
    apparmor_parser -r /etc/apparmor.d/geisten-desktop
fi
POSTINST
cat > "$stage/DEBIAN/prerm" <<'PRERM'
#!/bin/sh
set -eu
case "$1" in
    remove|deconfigure)
        if [ -r /sys/kernel/security/apparmor/profiles ] &&
           grep -q '^geisten-desktop (' /sys/kernel/security/apparmor/profiles; then
            apparmor_parser -R /etc/apparmor.d/geisten-desktop
        fi
        ;;
esac
PRERM
chmod 755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm"
(cd "$stage" && find usr -type f -exec md5sum {} + > DEBIAN/md5sums)
dpkg-deb --root-owner-group --build "$stage" "build/geisten-desktop_${version}_all.deb"
(cd build && sha256sum "geisten-desktop_${version}_all.deb" > "geisten-desktop_${version}_all.deb.sha256")
