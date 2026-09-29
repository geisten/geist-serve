#!/bin/sh
# Run inside an Alpine 3.23 container, with this repository as the cwd.
# Build-only dependencies; the final executable needs no package install.
set -eu
apk add --no-cache build-base python3 file pkgconf curl-dev curl-static openssl-dev c-ares-dev \
    openssl-libs-static brotli-static zstd-static zlib-static nghttp2-static \
    nghttp3-static ngtcp2-static libidn2-static libunistring-static libpsl-static
make -f App.mk app APP_CC=gcc APP_LDLIBS="$(pkg-config --static --libs libcurl openssl) -lpthread -static"
file geist-app
apk info -v > build/app-build-packages.txt
