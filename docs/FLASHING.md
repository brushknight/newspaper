# Flashing a release

Two installation options:

- [ESP32-S3](#esp32-s3): an ESP32-S3 board wired to the panel.
- [Raspberry Pi](#raspberry-pi): a Raspberry Pi Zero 2 W (or Zero W) with the Inky Impression 13.3" plugged onto its header, either with [the flashable image](#raspberry-pi-the-flashable-image) or with [Raspberry Pi OS and the install script](#raspberry-pi-raspberry-pi-os-and-installsh).

# ESP32-S3

Target: ESP32-S3 with 4 MB flash or more.

> The panel driver in this firmware is UNTESTED ON HARDWARE. The release binaries are built with the default pins listed in `docs/HARDWARE.md` of the repository. If your wiring differs, build from source and change the pins with `idf.py menuconfig`.

You need [esptool](https://docs.espressif.com/projects/esptool/): `pip install esptool`.

Put the board in download mode if it does not enter it by itself: hold BOOT, press RESET, release BOOT.

## One file

`newspaper-display-<version>-esp32s3-merged.bin` holds the bootloader, the partition table and the application. Write it at address 0:

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash 0x0 newspaper-display-<version>-esp32s3-merged.bin
```

On macOS the port looks like `/dev/tty.usbmodem101`, on Windows like `COM5`.

## Separate parts

The zip `newspaper-display-<version>-esp32s3-parts.zip` holds the three images and the `flash_args` file ESP-IDF produced:

| File | Address |
| --- | --- |
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `newspaper-display.bin` | `0x10000` |

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 newspaper-display.bin
```

To update only the application and keep Wi-Fi credentials and the pairing, write just `newspaper-display.bin` at `0x10000`.

## Starting over

To erase everything, including stored Wi-Fi credentials and the pairing token:

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_flash
```

Without a computer: hold the Home button while plugging the display in and keep it held for five seconds.

## After flashing

The display starts in Wi-Fi setup: the panel shows a QR code for the `Newspaper-XXXX` setup network. See "How pairing works" in the README.

# Raspberry Pi

> Untested on hardware. The program has only run in no-hardware mode, on a Mac and in Linux containers (including the ARMv6 release binary under emulation). The image has not been booted. See "What is tested and what is not" in the README.

## Raspberry Pi: the flashable image

`newspaper-display-<version>-rpizero2w.img.xz` (Raspberry Pi Zero 2 W) and `newspaper-display-<version>-rpizerow.img.xz` (the original Zero W) are small Linux systems built with Buildroot that start straight into the display. There is no desktop, no SSH and nothing else listening on the network.

### Write the card

Any microSD card of 1 GB or more. Check the download first: `sha256sum -c newspaper-display-<version>-rpizero2w.img.xz.sha256`.

- **Raspberry Pi Imager:** "Choose OS", "Use Custom", pick the `.img.xz`. Skip Imager's "OS customisation" (it does not apply to this image), and write.
- **dd** (Linux or macOS, replace `/dev/sdX` with your card, everything on it is lost):

  ```sh
  xz -dc newspaper-display-<version>-rpizero2w.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
  ```

  On macOS the card is `/dev/rdiskN` (see `diskutil list`; `diskutil unmountDisk /dev/diskN` first), and `bs=4m`.

### Tell it your Wi-Fi

Take the card out and put it back in: a drive called **NEWSPAPER** appears (the boot partition). Open `newspaper-wifi.txt` on it with any text editor and fill in three lines:

```
ssid=My home network
password=my wifi password
country=DE
```

- `ssid`: the network name, exactly as your phone shows it. `password`: empty for an open network. `country`: the two letter code of the country you are in (DE, GB, US, ...), which sets the allowed Wi-Fi channels.
- Put a value in double quotes if it starts or ends with a space. Comment lines start with `#`. Windows line ends are fine.
- The Pi Zero only has 2.4 GHz Wi-Fi. WPA2 and WPA2/WPA3 mixed networks work; WPA3 only networks do not.

Save, eject, put the card in the Pi, and power it.

How the file is used: at every start, `/etc/init.d/S41wifi` runs `newspaper-wifi`, which reads `newspaper-wifi.txt` (the boot partition is mounted read only at `/boot`) and writes `/run/wpa_supplicant.conf` in memory: the SSID in hex and the password turned into its 256-bit WPA key, so no character needs escaping and the password is not copied anywhere. `wpa_supplicant` joins the network and `udhcpc` gets an address. Nothing is written back to the card. To change networks, edit the file again and restart the display. If the file is still empty or has a mistake, the display shows "No Wi-Fi connection" with these instructions.

### What happens next

Within a minute or two of power on the panel shows the pairing screen (step 3 of the Quick start in the README). The token is kept on a small third partition (`/data`); the system itself is read only, so pulling the power is safe.

Card layout: 1. `NEWSPAPER` (FAT, 32 MB): Raspberry Pi firmware, kernel, `config.txt`, `newspaper-wifi.txt`. 2. root file system (ext4, 120 MB, mounted read only). 3. data (ext4, 16 MB): the pairing token and what the panel shows. The clock is set with NTP (`pool.ntp.org`); HTTPS waits until it is.

To start over without a computer: hold **Home** (button C) while the display starts and keep it held for five seconds; this erases the pairing. Writing the image again also erases it.

A login prompt exists on the HDMI and serial consoles (user `root`, no password), for debugging with a keyboard and screen. It is not reachable over the network.

Not in the image yet: the `Newspaper-XXXX` setup access point the ESP32 offers. Wi-Fi is set only through the text file. A captive portal on the Pi (wpa_supplicant in access point mode, a small DHCP and DNS server, and a setup page) is a planned follow-up.

## Raspberry Pi: Raspberry Pi OS and install.sh

For Raspberry Pi OS Lite (or Desktop), Bookworm or newer, 32 or 64 bit.

1. Write Raspberry Pi OS Lite with Raspberry Pi Imager. In "OS customisation" set your Wi-Fi, a user, and turn on SSH. The system owns Wi-Fi; the display program does not need its own setup.
2. Plug the Inky Impression onto the header, boot, and log in.
3. Run the installer:

   ```sh
   curl -fsSL https://raw.githubusercontent.com/brushknight/newspaper/main/targets/linux/install.sh | sudo bash
   ```

   or from a clone: `sudo ./targets/linux/install.sh`. Options: `--version TAG`, `--from-source`, `--api URL`, `--no-start`.
4. If it says so, reboot (`sudo reboot`): SPI was just turned on.

The installer can be run again at any time (for an update, for example). It:

- turns on SPI and adds `dtoverlay=spi0-0cs` to `/boot/firmware/config.txt` (keeping a copy as `config.txt.before-newspaper`),
- creates the system user `newspaper` in the groups `spi` and `gpio`,
- installs `/usr/local/bin/newspaper`: the release binary `newspaper-display-<version>-linux-armhf` or `-linux-arm64` for the system's architecture (checked against `SHA256SUMS.txt`), or a build from source when no release binary fits (this installs `build-essential cmake libcurl4-openssl-dev git` and takes a few minutes on a Zero),
- writes `/etc/newspaper/newspaper.conf` if there is none (all settings are listed in `targets/linux/newspaper.conf`),
- installs `newspaper.service`, which starts at boot, runs as `newspaper`, restarts on failure, and keeps the token in `/var/lib/newspaper` (mode 0700, file 0600).

The release binaries are static: `linux-armhf` is built for ARMv6 and runs on every Pi with 32-bit Raspberry Pi OS (the Zero W included), `linux-arm64` on 64-bit Raspberry Pi OS.

Useful commands:

```sh
journalctl -u newspaper -f              # the log
sudo systemctl restart newspaper        # restart (waits for a running refresh)
sudo ./targets/linux/uninstall.sh       # remove the program, keep the pairing
sudo ./targets/linux/uninstall.sh --purge   # also erase pairing, config, user and the config.txt lines
```

Without Wi-Fi the panel shows "No Wi-Fi connection" and how to fix it (Raspberry Pi Imager, or `sudo nmtui` on the Pi), and the program keeps trying.

## Raspberry Pi: building the image yourself

On a Linux machine with about 15 GB free:

```sh
targets/linux/image/build.sh rpizero2w ../newspaper-image/rpizero2w
```

It downloads Buildroot 2026.02.3 (checked against its published SHA-256) and builds with `targets/linux/image` as the external tree (`BR2_EXTERNAL`). The output directory must be outside the repository, because Buildroot copies the repository as the `newspaper` package. The first build takes one to two hours.
