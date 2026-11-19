#!/bin/bash
# Remove The Morning Newspaper display from Raspberry Pi OS.
#
#   sudo ./targets/linux/uninstall.sh            # keep the pairing and the config
#   sudo ./targets/linux/uninstall.sh --purge    # also erase them, the user, and the config.txt lines
#
# The panel keeps showing its last picture; e-ink needs no power for that.
# Also remove the display in the web app (or ask your assistant), or it stays
# listed on your account.
set -euo pipefail

main() {
	local purge=0
	case "${1:-}" in
	--purge) purge=1 ;;
	"") ;;
	*) echo "uninstall.sh: unknown option $1 (only --purge)" >&2; exit 2 ;;
	esac
	[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo bash uninstall.sh" >&2; exit 1; }

	if systemctl list-unit-files newspaper.service >/dev/null 2>&1; then
		# Stopping waits for a running panel refresh to end.
		systemctl disable --now newspaper.service 2>/dev/null || true
	fi
	rm -f /etc/systemd/system/newspaper.service /usr/local/bin/newspaper /etc/udev/rules.d/60-newspaper.rules
	systemctl daemon-reload
	rm -rf /run/newspaper
	echo "Removed the service and the program."

	if [ "$purge" -eq 1 ]; then
		rm -rf /var/lib/newspaper /etc/newspaper
		if id newspaper >/dev/null 2>&1; then
			userdel newspaper 2>/dev/null || true
		fi
		local c
		for c in /boot/firmware/config.txt /boot/config.txt; do
			if [ -f "$c" ] && grep -q '^# The Morning Newspaper display (install.sh)' "$c"; then
				sed -i '/^# The Morning Newspaper display (install.sh)$/,/^# end of The Morning Newspaper display$/d' "$c"
				echo "Removed the lines install.sh added to $c (SPI may still be on through raspi-config; reboot to apply)."
			fi
		done
		echo "Erased the pairing, the config and the user newspaper."
	else
		echo "Kept /etc/newspaper and /var/lib/newspaper (the pairing). Use --purge to erase them."
	fi
}

main "$@"
