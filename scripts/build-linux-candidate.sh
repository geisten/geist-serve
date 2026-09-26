#!/bin/sh
# Run in the pinned Alpine build container, with a read-only /source checkout.
set -eu
arch=${1:?amd64 or arm64}
apk add --no-cache build-base linux-headers git python3 file pkgconf curl-dev curl-static openssl-dev c-ares-dev openssl-libs-static brotli-static zstd-static zlib-static nghttp2-static nghttp3-static ngtcp2-static libidn2-static libunistring-static libpsl-static dpkg
mkdir -p /tmp/geist-build
cd /tmp/geist-build
cp -R /source/src /source/clients /source/scripts /source/web /source/tasks /source/quality /source/tests /source/App.mk /source/LICENSE /source/docs /source/deploy /source/geistlib .
# These are disposable copies. Host glibc/compiler objects must never be linked
# into the musl package even when make considers their timestamps current.
rm -rf geistlib/build geistlib/lib geistlib/bin
if [ "${REUSE_GEISTD:-0}" = 1 ]; then
    # Explicit local-only iteration flag; CI always rebuilds the pinned engine.
    cp /out/geistd ./geistd
else
    make -s -j2 -C geistlib lib TARGET=linux GEMM_PROVIDER=native MODE=release
    cc -std=c23 -O2 -D_GNU_SOURCE -Igeistlib/include -o geistd src/geistd.c src/template.c src/json.c src/net.c geistlib/lib/linux/release/libgeist.a -fopenmp -lm -static
fi
make -f App.mk app APP_CC=gcc APP_LDLIBS="$(pkg-config --static --libs libcurl openssl) -lpthread -static"
apk info -v > build/app-build-packages.txt
VERSION=${VERSION:-0.3.0} sh scripts/package-deb.sh "$arch"
cp geist geist-app geistd build/*.deb build/*.deb.sha256 build/app-build-packages.txt /out/
