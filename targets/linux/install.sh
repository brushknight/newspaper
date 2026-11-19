#!/bin/bash
# Install The Morning Newspaper display on Raspberry Pi OS (Bookworm or newer,
# Lite or Desktop, 32 or 64 bit) with a Pimoroni Inky Impression 13.3".
#
#   curl -fsSL https://raw.githubusercontent.com/brushknight/newspaper/main/targets/linux/install.sh | sudo bash
#   sudo ./targets/linux/install.sh            # from a clone
#
# Options:
#   --version TAG     install this release instead of the latest
#   --from-source     build from source even when a release binary exists
#   --api URL         platform address written to a new config file
#   --no-start        install and enable, but do not start the service now
#
# What it does, and it is safe to run again:
#   1. turns SPI on and adds dtoverlay=spi0-0cs to config.txt (Pimoroni's setup for the Inky)
#   2. creates the unprivileged user "newspaper" with access to SPI and GPIO
#   3. installs /usr/local/bin/newspaper: the release binary for this machine's
#      architecture (armhf on 32-bit Raspberry Pi OS, arm64 on
#      64-bit Raspberry Pi OS), or a build from source
#   4. writes /etc/newspaper/newspaper.conf if it does not exist yet
#   5. installs and enables newspaper.service (starts at boot, restarts on failure)
#
# Wi-Fi stays with the system: set it in Raspberry Pi Imager, or with nmtui.
set -euo pipefail

# Everything runs from main, so a "curl | bash" that breaks off midway runs nothing.
main() {
REPO="brushknight/newspaper"
BIN=/usr/local/bin/newspaper
CONF_DIR=/etc/newspaper
UNIT=/etc/systemd/system/newspaper.service
UDEV_RULE=/etc/udev/rules.d/60-newspaper.rules
MARK="# The Morning Newspaper display (install.sh)"

version=""
from_source=0
api=""
start=1
while [ $# -gt 0 ]; do
	case "$1" in
	--version) version="${2:?--version needs a tag}"; shift 2 ;;
	--from-source) from_source=1; shift ;;
	--api) api="${2:?--api needs a URL}"; shift 2 ;;
	--no-start) start=0; shift ;;
	-h | --help) sed -n '2,25p' "$0" 2>/dev/null || true; exit 0 ;;
	*) echo "install.sh: unknown option $1" >&2; exit 2 ;;
	esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root, for example: sudo bash install.sh"
command -v systemctl >/dev/null || die "systemd is needed (Raspberry Pi OS has it)"

# A clone has the sources next to this script; curl | bash has none.
here=""
if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then
	here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	[ -f "$here/CMakeLists.txt" ] && [ -f "$here/../../core/app.c" ] || here=""
fi

reboot_needed=0

# ---- 1. SPI and the chip select overlay ----
config_txt=""
for c in /boot/firmware/config.txt /boot/config.txt; do
	if [ -f "$c" ]; then config_txt="$c"; break; fi
done
if [ -n "$config_txt" ]; then
	say "Checking $config_txt"
	if ! grep -qE '^[[:space:]]*dtparam=spi=on' "$config_txt"; then
		if command -v raspi-config >/dev/null; then
			raspi-config nonint do_spi 0
		fi
	fi
	if ! grep -qE '^[[:space:]]*dtparam=spi=on' "$config_txt" || ! grep -qE '^[[:space:]]*dtoverlay=spi0-0cs' "$config_txt"; then
		[ -f "$config_txt.before-newspaper" ] || cp "$config_txt" "$config_txt.before-newspaper"
		local add=""
		grep -qE '^[[:space:]]*dtparam=spi=on' "$config_txt" || add="dtparam=spi=on"$'\n'
		# The Inky's chip selects are GPIO 26 and 16, moved by the program; keep SPI off GPIO 8 and 7.
		grep -qE '^[[:space:]]*dtoverlay=spi0-0cs' "$config_txt" || add="${add}dtoverlay=spi0-0cs"$'\n'
		printf '\n%s\n[all]\n%s# end of The Morning Newspaper display\n' "$MARK" "$add" >>"$config_txt"
		reboot_needed=1
		say "SPI settings added to $config_txt (a copy of the old file is $config_txt.before-newspaper)"
	fi
	[ -e /dev/spidev0.0 ] || reboot_needed=1
else
	echo "No config.txt found: not a Raspberry Pi? Turn on SPI yourself (dtparam=spi=on, dtoverlay=spi0-0cs)."
fi

# ---- 2. user and device access ----
say "Creating the user newspaper"
getent group spi >/dev/null || groupadd --system spi
getent group gpio >/dev/null || groupadd --system gpio
if ! id newspaper >/dev/null 2>&1; then
	useradd --system --user-group --no-create-home --home-dir /var/lib/newspaper \
		--shell /usr/sbin/nologin --comment "The Morning Newspaper display" newspaper
fi
usermod -a -G spi,gpio newspaper
# Raspberry Pi OS already gives these groups the devices; this rule makes sure on other systems.
cat >"$UDEV_RULE" <<'EOF'
# The Morning Newspaper display (install.sh): SPI and GPIO for the spi and gpio groups.
SUBSYSTEM=="spidev", GROUP="spi", MODE="0660"
SUBSYSTEM=="gpio", KERNEL=="gpiochip*", GROUP="gpio", MODE="0660"
EOF
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger --subsystem-match=spidev --subsystem-match=gpio 2>/dev/null || true

# ---- 3. the program ----
arch="$(dpkg --print-architecture 2>/dev/null || uname -m)"
case "$arch" in
armhf | armv6l | armv7l) asset_arch=armhf ;;
arm64 | aarch64) asset_arch=arm64 ;;
*) asset_arch="" ;;
esac

install_release() {
	command -v curl >/dev/null || apt-get install -y curl </dev/null
	local tag="$version"
	if [ -z "$tag" ]; then
		tag="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n 1)" || true
	fi
	[ -n "$tag" ] || return 1
	local name="newspaper-display-$tag-linux-$asset_arch"
	local base="https://github.com/$REPO/releases/download/$tag"
	local tmp
	tmp="$(mktemp -d)"
	say "Downloading $name"
	if ! curl -fsSL -o "$tmp/$name" "$base/$name" || ! curl -fsSL -o "$tmp/SHA256SUMS.txt" "$base/SHA256SUMS.txt"; then
		rm -rf "$tmp"
		echo "Release $tag has no $name."
		return 1
	fi
	if ! (cd "$tmp" && grep " $name\$" SHA256SUMS.txt | sha256sum -c --quiet -); then
		rm -rf "$tmp"
		die "checksum of $name does not match SHA256SUMS.txt"
	fi
	install -m 0755 "$tmp/$name" "$BIN.new"
	mv -f "$BIN.new" "$BIN"
	rm -rf "$tmp"
}

install_source() {
	say "Building from source (a few minutes on a Pi Zero)"
	apt-get update </dev/null
	apt-get install -y --no-install-recommends build-essential cmake pkg-config libcurl4-openssl-dev git ca-certificates </dev/null
	local src="$here/../.."
	local tmp=""
	if [ -z "$here" ]; then
		tmp="$(mktemp -d)"
		local ref=()
		[ -n "$version" ] && ref=(--branch "$version")
		git clone --depth 1 "${ref[@]}" "https://github.com/$REPO" "$tmp/src"
		src="$tmp/src"
	fi
	local build
	build="$(mktemp -d)"
	cmake -S "$src/targets/linux" -B "$build" -DCMAKE_BUILD_TYPE=Release
	cmake --build "$build" -j "$(nproc)"
	install -m 0755 "$build/newspaper" "$BIN.new"
	mv -f "$BIN.new" "$BIN"
	rm -rf "$build" ${tmp:+"$tmp"}
}

if [ "$from_source" -eq 0 ] && [ -n "$asset_arch" ] && install_release; then
	:
else
	[ -n "$asset_arch" ] || echo "No release binary for $arch."
	install_source
fi
say "Installed $BIN, version $("$BIN" --version)"

# ---- 4. config ----
mkdir -p "$CONF_DIR"
if [ ! -f "$CONF_DIR/newspaper.conf" ]; then
	if [ -n "$here" ] && [ -f "$here/newspaper.conf" ]; then
		cp "$here/newspaper.conf" "$CONF_DIR/newspaper.conf"
	else
		cat >"$CONF_DIR/newspaper.conf" <<'EOF'
# The Morning Newspaper display. All settings, with their defaults, are listed in
# https://github.com/brushknight/newspaper/blob/main/targets/linux/newspaper.conf
data_dir = /var/lib/newspaper
runtime_dir = /run/newspaper
EOF
	fi
	if [ -n "$api" ]; then
		echo "api = $api" >>"$CONF_DIR/newspaper.conf"
	fi
	chmod 0644 "$CONF_DIR/newspaper.conf"
	say "Wrote $CONF_DIR/newspaper.conf"
elif [ -n "$api" ]; then
	echo "Keeping the existing $CONF_DIR/newspaper.conf; set api = $api there yourself."
fi

# ---- 5. service ----
if [ -n "$here" ] && [ -f "$here/newspaper.service" ]; then
	install -m 0644 "$here/newspaper.service" "$UNIT"
else
	cat >"$UNIT" <<'EOF'
[Unit]
Description=The Morning Newspaper display
Documentation=https://github.com/brushknight/newspaper
After=network.target time-sync.target
Wants=network.target

[Service]
Type=simple
User=newspaper
Group=newspaper
SupplementaryGroups=spi gpio
ExecStart=/usr/local/bin/newspaper --config /etc/newspaper/newspaper.conf
Restart=on-failure
RestartSec=15
TimeoutStopSec=120
StateDirectory=newspaper
StateDirectoryMode=0700
RuntimeDirectory=newspaper
RuntimeDirectoryMode=0700
RuntimeDirectoryPreserve=yes
UMask=0077
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF
fi
systemctl daemon-reload
systemctl enable newspaper.service >/dev/null
say "newspaper.service is enabled and starts at boot"

if [ "$reboot_needed" -eq 1 ]; then
	echo
	echo "SPI was just turned on. Reboot to start the display:  sudo reboot"
elif [ "$start" -eq 1 ]; then
	systemctl restart newspaper.service
	say "Started. Follow it with: journalctl -u newspaper -f"
fi
echo
echo "The panel shows a pairing code once the Pi is online. Add the display at"
echo "https://newspaper.brushknight.com/app/pair, or ask your assistant to adopt it."
}

main "$@"
