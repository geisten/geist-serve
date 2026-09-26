#!/bin/sh
# OS launcher only. All model/download/runtime decisions belong to geist-app.
set -eu
cd "$(dirname "$0")"
opener=xdg-open
if [ "$(uname -s)" = Darwin ]; then opener=open; fi
if [ "$opener" != open ] && { [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || ! command -v xdg-open >/dev/null 2>&1; }; then
    echo 'Start an SSH tunnel from your computer: ssh -N -L 8766:127.0.0.1:8766 user@your-pi'
    echo 'Then open the private link printed below. Replace user@your-pi with your Pi login.'
    exec ./geist-app "$@"
fi
umask 077
output=$(mktemp)
./geist-app "$@" > "$output" &
app_pid=$!
cleanup() { kill -TERM "$app_pid" 2>/dev/null || true; wait "$app_pid" 2>/dev/null || true; rm -f "$output"; }
trap cleanup EXIT HUP INT TERM
i=0
while [ "$i" -lt 100 ]; do
    url=$(sed -n 's/^GEIST_APP_URL=//p' "$output")
    if [ -n "$url" ]; then
        "$opener" "$url" >/dev/null 2>&1 || printf 'Open this private link: %s\n' "$url"
        break
    fi
    kill -0 "$app_pid" 2>/dev/null || { cat "$output"; exit 1; }
    sleep .1
    i=$((i + 1))
done
wait "$app_pid"
