#!/bin/sh
# install.sh — geist-serve from the latest GitHub release, checksum-verified.
#
#   curl -fsSL https://raw.githubusercontent.com/geisten/geist-serve/main/install.sh | sh
#
# Installs the static binary to /usr/local/bin (or ~/.local/bin without
# root) and, on a systemd host as root, the socket-activated units. It never
# starts anything: the model path is yours to set. Override the download
# base with GEIST_SERVE_BASE (a file:// URL works; that is how tests run it)
# and the prefix with GEIST_SERVE_PREFIX.
set -eu

REPO="geisten/geist-serve"
BASE=${GEIST_SERVE_BASE:-"https://github.com/$REPO/releases/latest/download"}

case "$(uname -s)-$(uname -m)" in
Linux-x86_64)          ASSET=geist-serve-linux-x86_64 ;;
Linux-aarch64)         ASSET=geist-serve-linux-aarch64 ;;
Darwin-arm64)          ASSET=geist-serve-macos-arm64 ;;
*)
    echo "geist-serve: no prebuilt binary for $(uname -s) $(uname -m); build from source:" >&2
    echo "  git clone https://github.com/$REPO && cd geist-serve && make" >&2
    exit 1
    ;;
esac

if [ -n "${GEIST_SERVE_PREFIX:-}" ]; then PREFIX=$GEIST_SERVE_PREFIX
elif [ "$(id -u)" = 0 ]; then PREFIX=/usr/local
else PREFIX=$HOME/.local; fi
BIN="$PREFIX/bin"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "downloading $ASSET ..."
curl -fsSL --retry 3 -o "$TMP/$ASSET" "$BASE/$ASSET"
curl -fsSL --retry 3 -o "$TMP/SHA256SUMS" "$BASE/SHA256SUMS"
want=$(sed -n "s|^\([0-9a-f]\{64\}\) [ *]$ASSET\$|\1|p" "$TMP/SHA256SUMS" | head -1)
[ -n "$want" ] || { echo "geist-serve: $ASSET not listed in SHA256SUMS" >&2; exit 1; }
have=$( (sha256sum "$TMP/$ASSET" 2>/dev/null || shasum -a 256 "$TMP/$ASSET") | cut -d' ' -f1)
[ "$have" = "$want" ] || { echo "geist-serve: checksum mismatch for $ASSET" >&2; exit 1; }
echo "checksum ok"

mkdir -p "$BIN"
install -m 0755 "$TMP/$ASSET" "$BIN/geist-serve"
echo "installed $BIN/geist-serve"
# geistd (agents' socket daemon) ships next to it from v0.2; older releases have none.
if curl -fsSL --retry 3 -o "$TMP/$ASSET-geistd" "$BASE/$ASSET-geistd" 2>/dev/null; then
    want=$(sed -n "s|^\([0-9a-f]\{64\}\) [ *]$ASSET-geistd\$|\1|p" "$TMP/SHA256SUMS" | head -1)
    have=$( (sha256sum "$TMP/$ASSET-geistd" 2>/dev/null || shasum -a 256 "$TMP/$ASSET-geistd") | cut -d' ' -f1)
    [ "$have" = "$want" ] || { echo "geist-serve: checksum mismatch for $ASSET-geistd" >&2; exit 1; }
    install -m 0755 "$TMP/$ASSET-geistd" "$BIN/geistd"
    echo "installed $BIN/geistd"
fi
"$BIN/geist-serve" >/dev/null 2>&1 || [ $? -eq 2 ] || { echo "geist-serve: installed binary does not run" >&2; exit 1; }

# systemd units: only as root on a systemd host, and never overwriting a
# config file someone already edited.
if [ "$(id -u)" = 0 ] && [ -d /run/systemd/system ] && [ -z "${GEIST_SERVE_PREFIX:-}" ]; then
    for u in geist-serve.socket geist-serve.service; do
        curl -fsSL --retry 3 -o "/etc/systemd/system/$u" "$BASE/$u"
    done
    [ -e /etc/default/geist-serve ] || curl -fsSL --retry 3 -o /etc/default/geist-serve "$BASE/geist-serve.default"
    systemctl daemon-reload
    echo
    echo "systemd units installed. Put a GGUF at /var/lib/geist-serve/model.gguf"
    echo "(or edit GEIST_MODEL in /etc/default/geist-serve), then:"
    echo "  sudo systemctl enable --now geist-serve.socket"
    echo "  curl http://127.0.0.1:11434/api/tags"
else
    echo
    echo "run it:  $BIN/geist-serve model.gguf        # 127.0.0.1:11434"
    case ":$PATH:" in *":$BIN:"*) ;; *) echo "($BIN is not on your PATH)" ;; esac
fi
