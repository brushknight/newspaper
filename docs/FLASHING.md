# Flashing a release

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
