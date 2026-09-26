#!/bin/sh
# Destructive only to an ephemeral CI/container machine explicitly opted in.
set -eu
test "${GEIST_INSTALLER_TEST:-}" = 1
test "$(id -u)" = 0
package=$(realpath "${1:?path to candidate DEB}")
source_root=$(realpath "${2:?source checkout}")
apt-get update -qq
apt-get install -y -qq ca-certificates python3 systemd dbus-user-session desktop-file-utils procps >/dev/null
apt-get install -y -qq "$package" >/dev/null
desktop-file-validate /usr/share/applications/geist.desktop
systemd-analyze verify --man=no /usr/lib/systemd/user/geist.service
useradd -m geist-acceptance
uid=$(id -u geist-acceptance)
testroot=$(mktemp -d /tmp/geist-installed.XXXXXX)
cp -R "$source_root/tests" "$testroot/"
for binary in geist geist-app geistd; do ln -s "/usr/lib/geist/$binary" "$testroot/$binary"; done
chown -R geist-acceptance:geist-acceptance "$testroot"
runuser -u geist-acceptance -- env GEIST_TEST_MODEL="${GEIST_TEST_MODEL:-}" python3 "$testroot/tests/app/compat_test.py"
runuser -u geist-acceptance -- env GEIST_TEST_MODEL="${GEIST_TEST_MODEL:-}" python3 "$testroot/tests/app/cli_test.py"
as_user() {
    runuser -u geist-acceptance -- env XDG_RUNTIME_DIR="/run/user/$uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" "$@"
}
if test -d /run/systemd/system; then
    loginctl enable-linger geist-acceptance
    systemctl start "user@$uid.service"
    as_user geist start
    as_user systemctl --user enable geist.service
    as_user geist status >/dev/null
    before=$(as_user systemctl --user show -p MainPID --value geist.service)
    test "$before" -gt 1
    # Supervisor failure must recover through the installed unit.
    kill -KILL "$before"
    for attempt in $(seq 1 60); do
        after=$(as_user systemctl --user show -p MainPID --value geist.service)
        if test "$after" -gt 1 && test "$after" != "$before" && as_user geist status >/dev/null 2>&1; then break; fi
        sleep 1
    done
    test "$after" -gt 1
    test "$after" != "$before"
    as_user geist restart
    as_user geist status >/dev/null
else
    test "${GEIST_REQUIRE_SYSTEMD:-0}" != 1
    echo 'SKIPPED actual systemd lifecycle: container was not booted with systemd'
    as_user env GEIST_HOME=/home/geist-acceptance/.local/share/geist geist start
    as_user env GEIST_HOME=/home/geist-acceptance/.local/share/geist geist stop
fi
cp /home/geist-acceptance/.local/share/geist/api-key "$testroot/key-before"
echo 'user data' > /home/geist-acceptance/.local/share/geist/models/keep-me
# Build a version-only upgrade from the exact candidate, then roll back to it.
dpkg-deb -R "$package" "$testroot/upgrade"
version=$(dpkg-deb -f "$package" Version)
sed -i "s/^Version:.*/Version: $version+acceptance1/" "$testroot/upgrade/DEBIAN/control"
dpkg-deb --root-owner-group -b "$testroot/upgrade" "$testroot/upgrade.deb" >/dev/null
apt-get install -y -qq "$testroot/upgrade.deb" >/dev/null
if test -d /run/systemd/system; then
    as_user systemctl --user daemon-reload
    as_user geist restart
fi
cmp "$testroot/key-before" /home/geist-acceptance/.local/share/geist/api-key
apt-get install -y -qq --allow-downgrades "$package" >/dev/null
if test -d /run/systemd/system; then
    as_user systemctl --user daemon-reload
    as_user geist restart
fi
cmp "$testroot/key-before" /home/geist-acceptance/.local/share/geist/api-key
test "$(cat /home/geist-acceptance/.local/share/geist/models/keep-me)" = 'user data'
apt-get remove -y -qq geist >/dev/null
test ! -e /usr/bin/geist
if test -d /run/systemd/system; then
    ! as_user systemctl --user is-active --quiet geist.service
fi
test -f /home/geist-acceptance/.local/share/geist/models/keep-me
test ! -f /home/geist-acceptance/.local/share/geist/connection.json
cmp "$testroot/key-before" /home/geist-acceptance/.local/share/geist/api-key
echo 'PASS: candidate install, CLI/API, upgrade, rollback and removal; user data retained'
