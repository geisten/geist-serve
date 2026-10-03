#!/bin/sh
# Run only on an explicitly opted-in disposable native Ubuntu runner.
set -eu
test "${GEIST_INSTALLER_TEST:-}" = 1
test "$(id -u)" = 0
package=$(realpath "${1:?desktop candidate DEB}")
profile=/etc/apparmor.d/geist-desktop
loaded() { grep -q '^geist-desktop (' /sys/kernel/security/apparmor/profiles; }
loaded
test "$(stat -c '%U:%G:%a' "$profile")" = root:root:644
dpkg-query -W -f='${Conffiles}\n' geist-desktop | grep -F "$profile"
temporary=$(mktemp -d /tmp/geist-desktop-package.XXXXXX)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
# An administrator's conffile edit must survive reinstall/upgrade/rollback.
printf '\n# Acceptance-only administrator setting preservation test.\n' >> "$profile"
cp "$profile" "$temporary/expected"
apt-get install -y -qq --reinstall "$package"
loaded
cmp "$profile" "$temporary/expected"
dpkg-deb -R "$package" "$temporary/upgrade"
version=$(dpkg-deb -f "$package" Version)
sed -i "s/^Version:.*/Version: $version+acceptance1/" "$temporary/upgrade/DEBIAN/control"
dpkg-deb --root-owner-group -b "$temporary/upgrade" "$temporary/upgrade.deb"
apt-get install -y -qq "$temporary/upgrade.deb"
loaded
cmp "$profile" "$temporary/expected"
apt-get install -y -qq --allow-downgrades "$package"
loaded
cmp "$profile" "$temporary/expected"
apt-get remove -y -qq geist-desktop
if loaded; then echo "FAIL: profile remains loaded after removal" >&2; exit 1; fi
test ! -e /usr/bin/geist-desktop
test ! -e /usr/share/applications/geist.desktop
cmp "$profile" "$temporary/expected"
test -x /usr/bin/geisten && test -x /usr/bin/geist
apt-get install -y -qq "$package"
loaded
cmp "$profile" "$temporary/expected"
apt-get purge -y -qq geist-desktop
if loaded; then echo "FAIL: profile remains loaded after removal" >&2; exit 1; fi
test ! -e "$profile"
test -x /usr/bin/geisten && test -x /usr/bin/geist
echo 'PASS: desktop profile ownership, conffile, reinstall, upgrade, rollback, remove and purge; headless service retained'
