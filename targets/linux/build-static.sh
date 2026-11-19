#!/bin/sh
# Build fully static release binaries of the Linux program inside an Alpine
# container of the target architecture. Static musl binaries run on any
# Raspberry Pi OS of that architecture, whatever its glibc and libcurl.
#
#   docker run --rm --platform linux/arm/v6 -v "$PWD:/src" alpine:3.23 /src/targets/linux/build-static.sh /src/out/armhf
#   docker run --rm --platform linux/arm64  -v "$PWD:/src" alpine:3.23 /src/targets/linux/build-static.sh /src/out/arm64
#
# linux/arm/v6 is Alpine's armhf (ARMv6, hard float), so the 32-bit binary
# runs on any 32-bit Raspberry Pi OS. On an x86 machine Docker
# needs QEMU for these platforms (docker/setup-qemu-action in CI).
#
# Environment: NP_VERSION overrides the version string (default: VERSION file).
set -eu

out="${1:?usage: build-static.sh OUTPUT_DIR}"
src="$(cd "$(dirname "$0")/../.." && pwd)"
version="${NP_VERSION:-$(head -n 1 "$src/VERSION")}"

apk add --no-cache build-base cmake samurai pkgconf linux-headers file \
    curl-dev curl-static openssl-libs-static zlib-static nghttp2-static \
    brotli-static zstd-static libidn2-static libunistring-static libpsl-static c-ares-dev >/dev/null  # c-ares-dev carries libcares.a

build="$(mktemp -d)"
cmake -S "$src/targets/linux" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DNP_STATIC=ON -DNP_HARDWARE=ON -DNP_VERSION="$version"
cmake --build "$build"
strip "$build/newspaper" "$build/newspaper-wifi"
mkdir -p "$out"
cp "$build/newspaper" "$build/newspaper-wifi" "$out/"
file "$out/newspaper" "$out/newspaper-wifi"
"$out/newspaper" --version
ls -l "$out"
