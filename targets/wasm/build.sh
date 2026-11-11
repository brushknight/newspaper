#!/usr/bin/env bash
# Build the browser emulator into targets/wasm/dist (a static site).
# Needs Emscripten on the PATH:  source ~/emsdk/emsdk_env.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
dist="${1:-$here/dist}"
version="${NP_VERSION:-$(head -n 1 "$root/VERSION")}"

command -v emcc >/dev/null || { echo "emcc not found. Run: source ~/emsdk/emsdk_env.sh" >&2; exit 1; }

mkdir -p "$dist"
emcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
  -DNP_FIRMWARE_VERSION="\"$version\"" \
  -I"$root/core" -I"$root/third_party/qrcodegen" -I"$root/drivers/el133uf1" \
  "$here/main.c" \
  "$root/core/app.c" "$root/core/json.c" "$root/core/screen.c" "$root/core/fonts/np_fonts.c" \
  "$root/third_party/qrcodegen/qrcodegen.c" \
  "$root/drivers/el133uf1/el133uf1.c" \
  -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 \
  -sMODULARIZE -sEXPORT_NAME=createNewspaper -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=16MB \
  -sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8 \
  -o "$dist/newspaper.js"

cp "$here/web/index.html" "$here/web/emulator.js" "$here/web/panel-sim.js" "$dist/"
echo "$version" > "$dist/VERSION"
echo "emulator $version built in $dist"
ls -l "$dist"
