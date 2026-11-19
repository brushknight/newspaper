#!/bin/bash
# Buildroot post-image script: assemble sdcard.img with genimage. Adapted
# from Buildroot's board/raspberrypi/post-image.sh, with a third (data)
# partition and a labelled boot partition (see genimage.cfg.in).
set -euo pipefail

BOARD_DIR="$(dirname "$0")"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

FILES=()
for i in "${BINARIES_DIR}"/*.dtb "${BINARIES_DIR}"/rpi-firmware/*; do
	FILES+=("${i#"${BINARIES_DIR}"/}")
done
KERNEL=$(sed -n 's/^kernel=//p' "${BINARIES_DIR}/rpi-firmware/config.txt")
FILES+=("${KERNEL}")

BOOT_FILES=$(printf '\\t\\t\\t"%s",\\n' "${FILES[@]}")
sed "s|#BOOT_FILES#|${BOOT_FILES}|" "${BOARD_DIR}/genimage.cfg.in" >"${GENIMAGE_CFG}"

# An empty root path: genimage only places the prebuilt images, and the data partition starts empty.
ROOTPATH_TMP="$(mktemp -d)"
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${ROOTPATH_TMP}" \
	--tmppath "${GENIMAGE_TMP}" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"
