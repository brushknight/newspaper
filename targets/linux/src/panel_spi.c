/*
 * The panel on a Raspberry Pi header: spidev for clock and data, GPIO lines
 * for the two chip selects, D/C, RESET and BUSY. This is the bus the
 * portable driver in drivers/el133uf1 runs on, the same as on the ESP32.
 *
 * Wiring follows Pimoroni's inky/inky_el133uf1.py: SPI0 (MOSI GPIO 10,
 * SCLK GPIO 11) through /dev/spidev0.0 at 10 MHz, mode 0, and chip selects
 * on plain GPIOs 26 and 16 moved by the driver. The kernel's own chip select
 * (GPIO 8) is not connected to the panel; Pimoroni asks for
 * dtoverlay=spi0-0cs so the SPI driver does not claim it.
 *
 * UNTESTED ON HARDWARE, like the driver itself.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "gpio.h"
#include "platform.h"

#define CHUNK 4096 /* spidev's default bufsiz */

/* Bit positions in the output request. */
#define OUT_CS0 0
#define OUT_CS1 1
#define OUT_DC 2
#define OUT_RST 3

static int spi_fd = -1;
static int chip_fd = -1;
static int out_fd = -1;
static int busy_fd = -1;
static uint32_t spi_hz;

static void set_out(int bit, int level)
{
    gpio_set(out_fd, 1ULL << bit, level ? 1ULL << bit : 0);
}

static void bus_set_reset(void *ctx, int level)
{
    set_out(OUT_RST, level);
}

static void bus_set_cs(void *ctx, unsigned mask)
{
    /* Active low: a selected controller's line goes low. Both lines change in one call. */
    uint64_t bits = ((mask & EL133UF1_CS0) ? 0 : 1ULL << OUT_CS0) | ((mask & EL133UF1_CS1) ? 0 : 1ULL << OUT_CS1);
    gpio_set(out_fd, (1ULL << OUT_CS0) | (1ULL << OUT_CS1), bits);
}

static void bus_set_dc(void *ctx, int level)
{
    set_out(OUT_DC, level);
}

static void bus_write(void *ctx, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t n = len > CHUNK ? CHUNK : len;
        struct spi_ioc_transfer t;
        memset(&t, 0, sizeof(t));
        t.tx_buf = (unsigned long)data;
        t.len = (uint32_t)n;
        t.speed_hz = spi_hz;
        t.bits_per_word = 8;
        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) < 0) {
            np_log(NP_LOG_ERROR, "panel: spi transfer failed: %s", strerror(errno));
            return;
        }
        data += n;
        len -= n;
    }
}

static int bus_read_busy(void *ctx)
{
    uint64_t bits = 1; /* read as "ready" if the line cannot be read, so nothing waits forever */
    gpio_get(busy_fd, 1, &bits);
    return (int)(bits & 1);
}

static void bus_delay(void *ctx, uint32_t ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static int fail(const char *what, const char *detail)
{
    np_log(NP_LOG_ERROR, "panel: %s (%s)", what, detail);
    panel_spi_release();
    return -1;
}

int panel_spi_init(el133uf1_bus_t *bus, const np_linux_config_t *cfg)
{
    char chip_name[96] = "";
    spi_hz = cfg->spi_hz;
    spi_fd = open(cfg->spi_device, O_RDWR | O_CLOEXEC);
    if (spi_fd < 0) {
        np_log(NP_LOG_ERROR, "panel: cannot open %s: %s. Is SPI on (dtparam=spi=on in config.txt) and is this user in "
                             "the spi group?",
               cfg->spi_device, strerror(errno));
        return -1;
    }
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_hz) < 0) {
        return fail("cannot configure SPI", strerror(errno));
    }

    chip_fd = gpio_open_chip(cfg->gpiochip, chip_name, sizeof(chip_name));
    if (chip_fd < 0) {
        return fail("cannot open the GPIO chip; is this user in the gpio group?", strerror(errno));
    }
    unsigned outs[4] = { (unsigned)cfg->pin_cs0, (unsigned)cfg->pin_cs1, (unsigned)cfg->pin_dc,
                         (unsigned)cfg->pin_reset };
    /* Idle levels as in the Pimoroni driver: chip selects high, DC low, RESET high. */
    uint64_t initial = (1ULL << OUT_CS0) | (1ULL << OUT_CS1) | (1ULL << OUT_RST);
    out_fd = gpio_request_outputs(chip_fd, outs, 4, initial, "newspaper-panel");
    if (out_fd < 0) {
        return fail("cannot claim the panel's output lines (CS0, CS1, DC, RESET); another program may hold them, or "
                    "SPI claims a chip select (add dtoverlay=spi0-0cs)",
                    strerror(errno));
    }
    /* Pull-up as in the Pimoroni driver: a missing panel then reads "ready", not "busy forever". */
    busy_fd = gpio_request_input(chip_fd, (unsigned)cfg->pin_busy, true, "newspaper-busy");
    if (busy_fd < 0) {
        return fail("cannot claim the BUSY line", strerror(errno));
    }

    bus->ctx = NULL;
    bus->set_reset = bus_set_reset;
    bus->set_cs = bus_set_cs;
    bus->set_dc = bus_set_dc;
    bus->write = bus_write;
    bus->read_busy = bus_read_busy;
    bus->delay_ms = bus_delay;
    np_log(NP_LOG_INFO, "panel: EL133UF1 on %s at %u Hz, %s, CS0 %d CS1 %d DC %d RESET %d BUSY %d (driver untested on "
                        "hardware)",
           cfg->spi_device, (unsigned)spi_hz, chip_name, cfg->pin_cs0, cfg->pin_cs1, cfg->pin_dc, cfg->pin_reset,
           cfg->pin_busy);
    return 0;
}

void panel_spi_release(void)
{
    if (out_fd >= 0) {
        /* Leave the chip selects and reset high, as the ESP32 does before it sleeps. */
        gpio_set(out_fd, (1ULL << OUT_CS0) | (1ULL << OUT_CS1) | (1ULL << OUT_RST),
                 (1ULL << OUT_CS0) | (1ULL << OUT_CS1) | (1ULL << OUT_RST));
        close(out_fd);
        out_fd = -1;
    }
    if (busy_fd >= 0) {
        close(busy_fd);
        busy_fd = -1;
    }
    if (chip_fd >= 0) {
        close(chip_fd);
        chip_fd = -1;
    }
    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }
}
