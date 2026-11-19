#!/bin/bash
# Build the flashable Raspberry Pi image with Buildroot (pinned release below)
# and this directory as BR2_EXTERNAL.
#
#   targets/linux/image/build.sh [rpizero2w] [OUTPUT_DIR]
#
# Needs a Linux machine with Buildroot's usual host tools (build-essential,
# rsync, bc, cpio, unzip, file, wget, python3, perl) and about 15 GB of disk.
# The first build takes one to two hours. Output: OUTPUT_DIR/images/sdcard.img,
# and newspaper-display-<version>-<board>.img.xz next to it.
#
# Environment:
#   BR2_DL_DIR                  download cache (default: <output>/../dl)
#   NEWSPAPER_FIRMWARE_VERSION  version string of the program (default: VERSION file)
#   NP_CCACHE=1                 build with ccache in ~/.buildroot-ccache (CI caches it)
set -euo pipefail

BUILDROOT_VERSION=2026.02.3
BUILDROOT_SHA256=5a59e7501b0b4ec52c41f4bfa79412320e0b37eae5f719605a258e8d0c6fc7fb

board="${1:-rpizero2w}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="$(mkdir -p "${2:-$root/../newspaper-image/$board}" && cd "${2:-$root/../newspaper-image/$board}" && pwd)"
defconfig="newspaper_${board}_defconfig"
[ -f "$here/configs/$defconfig" ] || { echo "unknown board $board (only rpizero2w)" >&2; exit 2; }
case "$out/" in
"$root"/*) echo "put the output outside the repository ($out is inside it): Buildroot copies the repository as a package" >&2; exit 2 ;;
esac

work="$(dirname "$out")"
export BR2_DL_DIR="${BR2_DL_DIR:-$work/dl}"
mkdir -p "$BR2_DL_DIR"
src="$work/buildroot-$BUILDROOT_VERSION"
if [ ! -d "$src" ]; then
	tarball="$BR2_DL_DIR/buildroot-$BUILDROOT_VERSION.tar.xz"
	[ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://buildroot.org/downloads/buildroot-$BUILDROOT_VERSION.tar.xz"
	echo "$BUILDROOT_SHA256  $tarball" | sha256sum -c -
	tar -C "$work" -xf "$tarball"
fi

version="${NEWSPAPER_FIRMWARE_VERSION:-$(head -n 1 "$root/VERSION")}"
make -C "$src" O="$out" BR2_EXTERNAL="$here" "$defconfig"
if [ "${NP_CCACHE:-0}" = 1 ]; then
	echo 'BR2_CCACHE=y' >>"$out/.config"
	make -C "$src" O="$out" olddefconfig
fi
make -C "$src" O="$out" NEWSPAPER_FIRMWARE_VERSION="$version"

img="$out/images/newspaper-display-$version-$board.img"
cp "$out/images/sdcard.img" "$img"
xz -T0 -9 -f "$img"
(cd "$out/images" && sha256sum "$(basename "$img").xz" >"$(basename "$img").xz.sha256")
ls -l "$out/images"
