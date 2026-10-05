#!/bin/sh
# Run in the pinned Alpine build container, with a read-only /source checkout.
set -eu
arch=${1:?amd64 or arm64}
apk add --no-cache build-base linux-headers git python3 file pkgconf curl-dev curl-static openssl-dev c-ares-dev openssl-libs-static brotli-static zstd-static zlib-static nghttp2-static nghttp3-static ngtcp2-static libidn2-static libunistring-static libpsl-static dpkg
mkdir -p /tmp/geist-build
cd /tmp/geist-build
cp -R /source/src /source/clients /source/scripts /source/web /source/models /source/tasks /source/quality /source/tests /source/App.mk /source/runtime.mk /source/LICENSE /source/docs /source/deploy /source/geistlib /source/geist-runtime .
mkdir -p workbench && cp -R /source/workbench/suite workbench/  # the reference test suite id (#102)
# These are disposable copies. Host glibc/compiler objects must never be linked
# into the musl package even when make considers their timestamps current.
rm -rf geistlib/build geistlib/lib geistlib/bin geist-runtime/build
if [ "${REUSE_GEISTD:-0}" = 1 ]; then
    # Explicit local-only iteration flag; CI always rebuilds the pinned engine.
    cp /out/geistd ./geistd
else
    source_id=$(python3 scripts/engine-provenance.py identity geistlib)
    make -s -j2 -C geistlib lib TARGET=linux GEMM_PROVIDER=native MODE=release
    python3 scripts/engine-provenance.py capture geistlib --archive geistlib/lib/linux/release/libgeist.a --expected "$source_id" --output build/engine-build.h
    # Intentional word splitting: the repository manifest contains source paths.
    # geistd's chat ops run on geist-runtime, compiled against this engine (#148).
    make -s -C geist-runtime build/runtime.o core CC=cc GEISTLIB="$PWD/geistlib" ENGINE_LIB=
    cc -std=c23 -O2 -D_GNU_SOURCE -Ibuild -Igeistlib/include -Igeist-runtime/include -o geistd $(cat scripts/daemon-sources.list) geist-runtime/build/runtime.o geist-runtime/build/libgeistr-core.a geistlib/lib/linux/release/libgeist.a -fopenmp -lm -lpthread -static
fi
make -f App.mk app APP_CC=gcc APP_LDLIBS="$(pkg-config --static --libs libcurl openssl) -lpthread -static"
apk info -v > build/app-build-packages.txt
VERSION=${VERSION:-0.3.0} sh scripts/package-deb.sh "$arch"
cp geisten geist-app geistd build/*.deb build/*.deb.sha256 build/app-build-packages.txt /out/
