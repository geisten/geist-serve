#!/bin/sh
# The terminal client reuses the per-user service and opens its private UI.
set -eu
cd "$(dirname "$0")"
if [ "$#" -gt 0 ]; then exec ./geist-app "$@"; fi
exec ./geist open
