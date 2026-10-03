#!/bin/sh
# bootstrap_acceptance.sh DIR VERSION MODEL — the real portable archive in DIR
# through scripts/install-geist.sh (throwaway signing key, local test origin),
# then real inference, an idempotent rerun and uninstall (#46). Runs as a
# regular user with no geisten package installed; CI calls it before the DEB tests.
set -eu
cd "$(dirname "$0")/../.."
dir=${1:?usage: bootstrap_acceptance.sh DIR VERSION MODEL} version=${2:?} model=${3:?}
case $(uname -m) in
    x86_64) platform=linux-x86_64 ;;
    aarch64) platform=linux-aarch64 ;;
    *) echo "unsupported machine $(uname -m)" >&2; exit 1 ;;
esac
t=$(mktemp -d)
home=$t/home
geist=$home/.local/bin/geisten
cleanup() {
    [ -x "$geist" ] && HOME=$home "$geist" stop >/dev/null 2>&1
    rm -rf "$t"
}
trap cleanup EXIT
mkdir -p "$t/release" "$home"
cp "$dir/geist-$version-$platform.tar.gz" "$t/release/"
engine=$(sed -n 's/^GEIST_REF *?= *//p' Makefile)
python3 scripts/installer-manifest.py "$t/release" "$version" "$(git rev-parse HEAD)" "$engine" "$platform"
openssl genpkey -algorithm ed25519 -out "$t/key.pem" 2>/dev/null
openssl pkey -in "$t/key.pem" -pubout -out "$t/key.pub"
sh scripts/sign-manifest.sh "$t/release/geist-manifest" "$t/key.pem"

export HOME="$home" GEIST_INSTALL_TEST_ORIGIN="file://$t/release" GEIST_INSTALL_TEST_PUBKEY="$t/key.pub"
# `[ … ] && ok` would not stop a set -e script when the test fails: check exits.
check() { desc=$1; shift; if "$@"; then echo "ok   $desc"; else echo "FAIL $desc" >&2; exit 1; fi; }

sh scripts/install-geist.sh --no-start
check "installed rootless, launcher linked" [ "$(readlink "$geist")" = "$home/.local/share/geist-runtime/current/geisten" ]
check "the earlier name geist still works" [ "$(readlink "$home/.local/bin/geist")" = "$home/.local/share/geist-runtime/current/geisten" ]

GEIST_MODEL="$model" "$geist" start
for _ in $(seq 1 120); do
    "$geist" status >"$t/status" 2>/dev/null && grep -q '"ready":true' "$t/status" && break
    sleep 1
done
check "service ready with the test model" grep -q '"ready":true' "$t/status"
check "real generation through the installed CLI" "$geist" test
before=$("$geist" connection)

sh scripts/install-geist.sh
check "same-version rerun keeps the running service and its key" [ "$("$geist" connection)" = "$before" ]

sh scripts/install-geist.sh --uninstall
check "uninstall removed the launcher" [ ! -e "$geist" ]
check "uninstall removed the runtime" [ ! -e "$home/.local/share/geist-runtime" ]
check "service data kept after uninstall" [ -d "$home/.local/share/geisten" ]
echo "bootstrap acceptance: all passed"
