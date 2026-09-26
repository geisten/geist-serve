#!/bin/sh
# Destructive only to an ephemeral CI/container machine explicitly opted in.
set -eu
test "${GEIST_INSTALLER_TEST:-}" = 1
test "$(id -u)" = 0
package=$(realpath "${1:?path to candidate DEB}")
source_root=$(realpath "${2:?source checkout}")
(cd "$(dirname "$package")" && sha256sum -c "$(basename "$package").sha256")
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
if test -n "${GEIST_TEST_MODEL:-}"; then
    # The runner's private checkout/cache need not be traversable by this user.
    # Copy only the public, verified model fixture; do not relax checkout modes.
    install -o geist-acceptance -g geist-acceptance -m 600 "$GEIST_TEST_MODEL" "$testroot/model.gguf"
    GEIST_TEST_MODEL="$testroot/model.gguf"
    export GEIST_TEST_MODEL
    runuser -u geist-acceptance -- test -r "$GEIST_TEST_MODEL"
fi
runuser -u geist-acceptance -- env GEIST_TEST_MODEL="${GEIST_TEST_MODEL:-}" python3 "$testroot/tests/app/compat_test.py"
runuser -u geist-acceptance -- env GEIST_TEST_MODEL="${GEIST_TEST_MODEL:-}" python3 "$testroot/tests/app/cli_test.py"
as_user() {
    runuser -u geist-acceptance -- env XDG_RUNTIME_DIR="/run/user/$uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" "$@"
}
check_selected_model() {
    test -n "${GEIST_TEST_MODEL:-}" || return 0
    for attempt in $(seq 1 60); do
        if as_user geist status | python3 -c 'import json,sys; s=json.load(sys.stdin); sys.exit(not(s["ready"] and s["active_id"]=="smollm2-360m"))'; then
            as_user geist test | python3 -c 'import json,sys; r=json.load(sys.stdin); assert r["usage"]["completion_tokens"]>0'
            echo "PASS: selected model answers through installed systemd service after $1"
            return 0
        fi
        sleep 1
    done
    echo "FAIL: selected model did not recover after $1" >&2
    as_user journalctl --user -u geist.service --no-pager -n 40 >&2
    return 1
}
if test -d /run/systemd/system; then
    # Required native acceptance must exercise the actual sandboxed service with
    # its selected catalog model, not only a foreground --model fixture.
    if test "${GEIST_REQUIRE_SYSTEMD:-0}" = 1; then test -n "${GEIST_TEST_MODEL:-}"; fi
    loginctl enable-linger geist-acceptance
    systemctl start "user@$uid.service"
    as_user geist start
    as_user systemctl --user enable geist.service
    as_user geist status >/dev/null
    if test -n "${GEIST_TEST_MODEL:-}"; then
        install -o geist-acceptance -g geist-acceptance -m 600 "$GEIST_TEST_MODEL" \
            /home/geist-acceptance/.local/share/geist/models/smollm2-360m-instruct-q8_0.gguf
        as_user geist use smollm2-360m >/dev/null
        check_selected_model 'initial selection'
    fi
    cp /home/geist-acceptance/.local/share/geist/api-key "$testroot/key-before"
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
    check_selected_model 'supervisor crash'
    as_user geist restart
    as_user geist status >/dev/null
    check_selected_model 'explicit restart'
    cmp "$testroot/key-before" /home/geist-acceptance/.local/share/geist/api-key
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
    check_selected_model 'package upgrade'
fi
cmp "$testroot/key-before" /home/geist-acceptance/.local/share/geist/api-key
apt-get install -y -qq --allow-downgrades "$package" >/dev/null
if test -d /run/systemd/system; then
    as_user systemctl --user daemon-reload
    as_user geist restart
    check_selected_model 'package rollback'
fi
cmp "$testroot/key-before" /home/geist-acceptance/.local/share/geist/api-key
test "$(cat /home/geist-acceptance/.local/share/geist/models/keep-me)" = 'user data'
apt-get remove -y -qq geist >/dev/null
test ! -e /usr/bin/geist
if test -d /run/systemd/system; then
    ! as_user systemctl --user is-active --quiet geist.service
    ! as_user systemctl --user is-enabled --quiet geist.service
    if test -n "${GEIST_TEST_MODEL:-}"; then
        cmp "$GEIST_TEST_MODEL" /home/geist-acceptance/.local/share/geist/models/smollm2-360m-instruct-q8_0.gguf
    fi
fi
test -f /home/geist-acceptance/.local/share/geist/models/keep-me
test ! -f /home/geist-acceptance/.local/share/geist/connection.json
cmp "$testroot/key-before" /home/geist-acceptance/.local/share/geist/api-key
echo 'PASS: candidate install, CLI/API, upgrade, rollback and removal; user data retained'
