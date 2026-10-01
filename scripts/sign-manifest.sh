#!/bin/sh
# sign-manifest.sh MANIFEST KEY.pem: write MANIFEST.sig, the base64 Ed25519
# signature over the exact manifest bytes (docs/INSTALL-LINUX.md, "Signing").
# Release CI passes the protected signing key; tests pass a fixture key.
set -eu
manifest=${1:?usage: sign-manifest.sh MANIFEST KEY.pem}
key=${2:?usage: sign-manifest.sh MANIFEST KEY.pem}
raw=$(mktemp)
trap 'rm -f "$raw"' EXIT
openssl pkeyutl -sign -inkey "$key" -rawin -in "$manifest" -out "$raw"
[ "$(wc -c <"$raw")" -eq 64 ] || { echo "sign-manifest: not an Ed25519 key" >&2; exit 1; }
base64 <"$raw" | tr -d '\n' >"$manifest.sig"
printf '\n' >>"$manifest.sig"
