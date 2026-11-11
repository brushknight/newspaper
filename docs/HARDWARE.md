# Hardware

> **UNTESTED ON HARDWARE.** This wiring comes from the Kconfig defaults and from reading the reference drivers. Nobody has connected it yet. Check every line against your board's own documentation before applying power.

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

## Frame orientation

The platform sends frames in the layout described in [PROTOCOL.md](PROTOCOL.md) section 4: 1200 x 1600 portrait, left 600 columns first, then the right 600. Both reference drivers send the panel exactly that: the left half to the first controller (CS0) and the right half to the second (CS1), top row first. So the driver passes the bytes through without rotation or mirroring. If the first picture on real hardware turns out mirrored or upside down, `drivers/el133uf1/el133uf1.c` is the place to fix it; the platform's layout does not change.

## Power

The firmware assumes mains (USB) power and reports no battery level. Between activities it enters deep sleep with a timer and the three buttons as wake sources, and holds the chip selects and reset high so the sleeping panel is not disturbed. The panel itself draws no power while it shows a picture.

## Panel limits the firmware respects

- A refresh takes about 12 to 20 seconds and is never interrupted. Button presses during it are ignored.
- At least 180 seconds pass between the end of one refresh and the start of the next (`EL133UF1_MIN_REFRESH_INTERVAL_MS`).
- No partial refresh.
