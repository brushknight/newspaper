# Hardware

> **UNTESTED ON HARDWARE.** This wiring comes from the Kconfig defaults and from reading the reference drivers. Nobody has connected it yet. Check every line against your board's own documentation before applying power.

There are two ways to drive the panel: an ESP32-S3 wired to it (below), or a Raspberry Pi that the Inky Impression plugs onto directly ([Raspberry Pi](#raspberry-pi)). Both run the same core and panel driver.

## Parts

| Part | Notes |
| --- | --- |
| ESP32-S3 board | Any module with 4 MB flash or more. PSRAM is not needed. The default pins suit an ESP32-S3-DevKitC-1. |
| Pimoroni Inky Impression 13.3" | E Ink Spectra 6, panel EL133UF1, 1200 x 1600. It has a Raspberry Pi style 40 pin header. |
| 3 push buttons | Next, Back, Home. Each connects its GPIO to GND. The Inky Impression has four buttons of its own on the header that can serve. |

## Wiring

All GPIO numbers are defaults. Change them with `idf.py menuconfig` under "Morning Newspaper display"; the option name is in the last column.

### Panel

| Signal | ESP32-S3 GPIO (default) | Inky Impression 13.3" header | Direction | Kconfig option |
| --- | --- | --- | --- | --- |
| SPI clock (SCLK) | 12 | pin 23 (Pi GPIO 11) | out | `NP_PIN_PANEL_SCLK` |
| SPI data (MOSI) | 11 | pin 19 (Pi GPIO 10) | out | `NP_PIN_PANEL_MOSI` |
| Chip select 0, left half | 10 | pin 37 (Pi GPIO 26) | out, active low | `NP_PIN_PANEL_CS0` |
| Chip select 1, right half | 9 | pin 36 (Pi GPIO 16) | out, active low | `NP_PIN_PANEL_CS1` |
| Data/command (DC) | 14 | pin 15 (Pi GPIO 22) | out | `NP_PIN_PANEL_DC` |
| Reset (RST) | 13 | pin 13 (Pi GPIO 27) | out, active low | `NP_PIN_PANEL_RST` |
| BUSY | 8 | pin 11 (Pi GPIO 17) | in, low while busy | `NP_PIN_PANEL_BUSY` |
| Panel power enable | not used (-1) | not present on the Inky | out | `NP_PIN_PANEL_POWER` |
| 3V3, 5V, GND | 3V3, 5V, GND | pins 1, 2, 6 | power | |

The header pin numbers are the constants in Pimoroni's `inky_el133uf1.py` (`MOSI 10, SCLK 11, CS0 26, CS1 16, DC 22, RESET 27, BUSY 17`, as Raspberry Pi GPIO numbers) translated to physical pins. Confirm them and the power pins with Pimoroni's pinout for the board.

The panel has no MISO line in use; the driver only writes. SPI runs in mode 0 at 10 MHz by default (`NP_PANEL_SPI_HZ`). The two chip selects are plain GPIOs moved by the driver, because some commands go to both controllers at once and pixel data goes to one at a time.

The Waveshare 13.3inch e-Paper HAT+ (E) carries the same panel. It has a power enable line; set `NP_PIN_PANEL_POWER` to the GPIO you connect it to.

### Buttons

| Button | ESP32-S3 GPIO (default) | Kconfig option |
| --- | --- | --- |
| Next | 4 | `NP_PIN_BTN_NEXT` |
| Back | 5 | `NP_PIN_BTN_BACK` |
| Home | 6 | `NP_PIN_BTN_HOME` |

Each button connects its GPIO to GND. The firmware turns on the internal pull-up, debounces for 30 ms (`NP_BTN_DEBOUNCE_MS`), and uses the buttons to wake from deep sleep. Wake needs RTC capable pins, which on the ESP32-S3 are GPIO 0 to 21.

Holding Home while plugging the display in, for five seconds, erases Wi-Fi credentials and the pairing.

### Pins to avoid on the ESP32-S3

- GPIO 0, 3, 45, 46: strapping pins.
- GPIO 19, 20: USB.
- GPIO 26 to 32: flash. GPIO 33 to 37 as well on modules with octal flash or PSRAM.
- GPIO 43, 44: the serial console.

## Raspberry Pi

The Pimoroni Inky Impression 13.3" is a Raspberry Pi HAT: it plugs straight onto the 40 pin header, so there is nothing to wire. `targets/linux` runs the same core and panel driver as the ESP32 on Raspberry Pi OS or on the flashable image ([FLASHING.md](FLASHING.md)).

| Board | Status |
| --- | --- |
| Raspberry Pi Zero 2 W | the main target: release binaries (armhf and arm64) and a flashable image |
| Other Raspberry Pi boards | should work with Raspberry Pi OS and the install script (the program finds the header's GPIO chip by its label, Pi 5 included); not tried |

Untested on hardware, like everything here: the program has run only in no-hardware mode (see the README).

### Pins on the header

All numbers are BCM GPIO numbers (the line offsets on the header's GPIO chip). Each one is a setting in `/etc/newspaper/newspaper.conf` (last column), so a different HAT can be configured without a rebuild.

| Signal | BCM GPIO | Header pin | Direction | Setting |
| --- | --- | --- | --- | --- |
| SPI data (MOSI), SPI0 | 10 | 19 | out | (spidev) |
| SPI clock (SCLK), SPI0 | 11 | 23 | out | (spidev) |
| Chip select 0, left half | 26 | 37 | out, active low | `pin_cs0` |
| Chip select 1, right half | 16 | 36 | out, active low | `pin_cs1` |
| Data/command (DC) | 22 | 15 | out | `pin_dc` |
| Reset (RESET) | 27 | 13 | out, active low | `pin_reset` |
| BUSY | 17 | 11 | in, pull-up, low while busy | `pin_busy` |
| Button A: **Next** | 5 | 29 | in, pull-up, pressed = low | `button_next` |
| Button B: **Back** | 6 | 31 | in, pull-up, pressed = low | `button_back` |
| Button C: **Home** | 25 | 22 | in, pull-up, pressed = low | `button_home` |
| Button D: not used | 24 | 18 | (free) | |

SPI goes through `/dev/spidev0.0` (`spi_device`) in mode 0 at 10 MHz (`spi_hz`), 4096 bytes per transfer. The panel's two chip selects are plain GPIO lines that the driver moves itself; the kernel's own chip select line (GPIO 8, CE0) is not connected to the panel.

Sources, read from [Pimoroni's inky library](https://github.com/pimoroni/inky) at commit `a4264856f04c`:

- `inky/inky_el133uf1.py`: `RESET_PIN = 27  # PIN13`, `BUSY_PIN = 17  # PIN11`, `DC_PIN = 22  # PIN15`, `MOSI_PIN = 10`, `SCLK_PIN = 11`, `CS0_PIN = 26`, `CS1_PIN = 16`; BUSY requested with `Bias.PULL_UP`; chip selects and RESET start high, DC low; `spidev.open(0, 0)` with `max_speed_hz = 10000000`; writes in 4096 byte pieces.
- `examples/spectra6/buttons.py`: buttons A, B, C, D on GPIO 5, 6, 16, 24 from top to bottom, with the note `SW_C = 16  # Set this value to '25' if you're using a Impression 13.3"` (on the 13.3" GPIO 16 is chip select 1). Inputs with pull-up, falling edge.
- `README.md` and `pyproject.toml`: SPI on (`raspi-config nonint do_spi 0`) and `dtoverlay=spi0-0cs` in `/boot/firmware/config.txt` ("you may need to disable SPI's chip-select"). Pimoroni also turns on I2C (`dtoverlay=i2c1`) for the board's ID EEPROM, which this program does not read.
- The header pin numbers are the standard Raspberry Pi 40 pin layout.

Which button is Next, Back and Home is our choice: A, B and C in that order, D left free for later. Change it with the three `button_` settings.

### config.txt

```
dtparam=spi=on
dtoverlay=spi0-0cs
```

`install.sh` adds both (in an `[all]` section, marked so `uninstall.sh --purge` can remove it) and the flashable image ships with them. A reboot is needed after the first change. With `dtparam=spi=on` alone the SPI driver claims GPIO 8 and 7 as chip selects; `spi0-0cs` keeps it away from them, as Pimoroni asks.

### Access without root

The program runs as the user `newspaper`, a member of the groups `spi` and `gpio`. On Raspberry Pi OS the system already gives `/dev/spidev*` to `spi` and `/dev/gpiochip*` to `gpio`; `install.sh` also adds a udev rule that does the same. GPIO goes through the kernel's character device (uAPI v2, Linux 5.10 or newer), not sysfs and not libgpiod.

### Starting over on the Pi

Hold **Home** (button C) while the program starts (at boot, or `sudo systemctl restart newspaper`) and keep it held for five seconds: the pairing is erased. Wi-Fi belongs to the system and is not touched. From a shell, `sudo systemctl stop newspaper && sudo -u newspaper newspaper --forget --config /etc/newspaper/newspaper.conf && sudo systemctl start newspaper` does the same.

### Power and limits on the Pi

The Pi stays on; where the ESP32 deep-sleeps, the program simply waits. The panel rules are the same: a refresh is never interrupted (stopping the service waits for it), at least 180 seconds pass between refreshes, and the last page stays up through network trouble. The time of the last refresh is kept in `/run/newspaper`, so the 180 second rule also holds when the program is restarted.

## Frame orientation

The platform sends frames in the layout described in [PROTOCOL.md](PROTOCOL.md) section 4: 1200 x 1600 portrait, left 600 columns first, then the right 600. Both reference drivers send the panel exactly that: the left half to the first controller (CS0) and the right half to the second (CS1), top row first. So the driver passes the bytes through without rotation or mirroring. If the first picture on real hardware turns out mirrored or upside down, `drivers/el133uf1/el133uf1.c` is the place to fix it; the platform's layout does not change.

## Power

The firmware assumes mains (USB) power and reports no battery level. Between activities it enters deep sleep with a timer and the three buttons as wake sources, and holds the chip selects and reset high so the sleeping panel is not disturbed. The panel itself draws no power while it shows a picture.

## Panel limits the firmware respects

- A refresh takes about 12 to 20 seconds and is never interrupted. Button presses during it are ignored.
- At least 180 seconds pass between the end of one refresh and the start of the next (`EL133UF1_MIN_REFRESH_INTERVAL_MS`).
- No partial refresh.
