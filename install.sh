#!/bin/sh
# Legacy standalone server installer. For the model manager use the Geist
# Mac/Ubuntu package described in docs/INSTALL.md.
# All payloads are verified and staged before replacing any installed file.
set -eu
umask 077
REPO=geisten/geist-serve
BASE=${GEIST_SERVE_BASE:-https://github.com/$REPO/releases/latest/download}
case "$BASE" in https://*|file://*) ;; *) echo 'geist-serve: use HTTPS or a local file:// release' >&2; exit 1;; esac
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) ASSET=geist-serve-linux-x86_64 ;;
    Linux-aarch64) ASSET=geist-serve-linux-aarch64 ;;
    Darwin-arm64) ASSET=geist-serve-macos-arm64 ;;
    *) echo 'geist-serve: unsupported platform; build from source' >&2; exit 1;;
esac
if [ -n "${GEIST_SERVE_PREFIX:-}" ]; then PREFIX=$GEIST_SERVE_PREFIX
elif [ "$(id -u)" = 0 ]; then PREFIX=/usr/local
else PREFIX=$HOME/.local; fi
case "$PREFIX" in /*) ;; *) echo 'geist-serve: prefix must be absolute' >&2; exit 1;; esac
case "$PREFIX" in *'
'*) echo 'geist-serve: newline in prefix' >&2; exit 1;; esac
BIN=$PREFIX/bin
TMP=$(mktemp -d)
LOCK=
committed=0
units=0
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ -n "$LOCK" ]; then
        if [ "$committed" = 0 ]; then
            # Roll back ordinary failures/signals. Power loss/SIGKILL cannot be
            # made transactional across bin and /etc by this shell installer.
            for item in "$LOCK"/item-*; do
                [ -d "$item" ] || continue
                dest=$(cat "$item/destination")
                if [ -e "$item/changed" ]; then
                    if [ -e "$item/previous" ]; then
                        rollback_stage=$(cat "$item/staging-path")
                        cp -p "$item/previous" "$rollback_stage" && mv -f "$rollback_stage" "$dest" || {
                            echo "geist-serve: rollback failed; recovery files: $LOCK" >&2
                            exit 1
                        }
                    else rm -f "$dest"; fi
                fi
            done
            if [ "$units" = 1 ]; then
                systemctl daemon-reload || {
                    echo "geist-serve: restored files but service reload failed; inspect $LOCK" >&2
                    exit 1
                }
            fi
        fi
        # Remove only temporary sibling files created by this invocation.
        for item in "$LOCK"/item-*; do
            [ -d "$item" ] || continue
            [ ! -f "$item/staging-path" ] || rm -f "$(cat "$item/staging-path")"
        done
        rm -rf "$LOCK"
    fi
    rm -rf "$TMP"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
fetch() { curl -fSL --proto '=https,file' --proto-redir '=https' --connect-timeout 15 --max-time 300 --retry 3 -o "$TMP/$1" "$BASE/$1"; }
hash() { (sha256sum "$1" 2>/dev/null || shasum -a 256 "$1") | cut -d ' ' -f1; }
fetch SHA256SUMS
# Units are verified even for a user-prefix install, so every platform checks
# the complete release contract. Legacy incomplete manifests fail closed.
for name in "$ASSET" "$ASSET-geistd" geist-serve.socket geist-serve.service geist-serve.default; do
    want=$(awk -v name="$name" '$2 == name || $2 == "*" name {count++; digest=$1} END {if(count != 1) exit 1; print digest}' "$TMP/SHA256SUMS") || {
        echo "geist-serve: expected exactly one checksum for $name" >&2; exit 1;
    }
    [ "${#want}" = 64 ] || { echo 'geist-serve: invalid checksum' >&2; exit 1; }
    case "$want" in *[!0-9a-f]*) echo 'geist-serve: invalid checksum' >&2; exit 1;; esac
    fetch "$name"
    [ "$(hash "$TMP/$name")" = "$want" ] || { echo "geist-serve: checksum mismatch for $name" >&2; exit 1; }
done
for name in "$ASSET" "$ASSET-geistd"; do
    chmod 755 "$TMP/$name"
    status=0
    "$TMP/$name" >/dev/null 2>&1 || status=$?
    [ "$status" = 2 ] || { echo "geist-serve: $name failed the usage check" >&2; exit 1; }
done
mkdir -p "$BIN"
lock_path=$PREFIX/.geist-serve-install.lock
mkdir "$lock_path" 2>/dev/null || { echo "geist-serve: install locked; inspect $lock_path" >&2; exit 1; }
LOCK=$lock_path
n=0
stage() {
    source=$1; dest=$2; mode=$3
    [ ! -L "$dest" ] && { [ ! -e "$dest" ] || [ -f "$dest" ]; } || {
        echo "geist-serve: refusing non-regular target $dest" >&2; exit 1;
    }
    n=$((n + 1)); item=$LOCK/item-$n; mkdir "$item"
    printf '%s\n' "$dest" > "$item/destination"
    staging=$(mktemp "$(dirname "$dest")/.geist-install.XXXXXX")
    printf '%s\n' "$staging" > "$item/staging-path"
    if [ -f "$dest" ]; then cp -p "$dest" "$item/previous"; fi
    install -m "$mode" "$TMP/$source" "$staging"
}
stage "$ASSET" "$BIN/geist-serve" 755
stage "$ASSET-geistd" "$BIN/geistd" 755
units=0
if [ "$(id -u)" = 0 ] && [ -d /run/systemd/system ] && [ -z "${GEIST_SERVE_PREFIX:-}" ]; then
    units=1
    stage geist-serve.socket /etc/systemd/system/geist-serve.socket 644
    stage geist-serve.service /etc/systemd/system/geist-serve.service 644
    # Never replace existing owner configuration, including a symlink.
    if [ ! -e /etc/default/geist-serve ] && [ ! -L /etc/default/geist-serve ]; then
        stage geist-serve.default /etc/default/geist-serve 644
    fi
fi
for item in "$LOCK"/item-*; do
    dest=$(cat "$item/destination")
    touch "$item/changed"
    mv -f "$(cat "$item/staging-path")" "$dest"
done
if [ "$units" = 1 ]; then systemctl daemon-reload; fi
committed=1
printf 'Installed verified server and daemon in %s\n' "$BIN"
if [ "$units" = 1 ]; then
    echo 'Set GEIST_MODEL in /etc/default/geist-serve, then enable geist-serve.socket.'
else
    printf 'Run: %s/geist-serve model.gguf\n' "$BIN"
fi
