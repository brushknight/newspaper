#!/bin/sh
# Buildroot post-build script: mount points, fstab entries and the init
# entry that keeps the newspaper program running. Runs with TARGET_DIR set.
set -eu

mkdir -p "$TARGET_DIR/boot" "$TARGET_DIR/data"

fstab="$TARGET_DIR/etc/fstab"
# The FAT boot partition, read only: newspaper-wifi.txt is read from it at every start.
grep -q '^/dev/mmcblk0p1' "$fstab" ||
	echo '/dev/mmcblk0p1	/boot	vfat	ro,codepage=437,iocharset=ascii	0	0' >>"$fstab"
# The small data partition: the device token and what the panel shows.
grep -q '^/dev/mmcblk0p3' "$fstab" ||
	echo '/dev/mmcblk0p3	/data	ext4	rw,noatime	0	0' >>"$fstab"

inittab="$TARGET_DIR/etc/inittab"
# A login on the HDMI console too, as in Buildroot's Raspberry Pi boards.
grep -qE '^tty1::' "$inittab" ||
	sed -i '/GENERIC_SERIAL/a\
tty1::respawn:/sbin/getty -L  tty1 0 vt100 # HDMI console' "$inittab"
# The display program: BusyBox init starts it after the rc scripts and restarts it if it ever exits.
grep -q 'newspaper-run' "$inittab" ||
	printf '\n# The Morning Newspaper display\n::respawn:/usr/libexec/newspaper-run\n' >>"$inittab"
