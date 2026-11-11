/*
 * Driver for the EL133UF1 panel: 13.3 inch E Ink Spectra 6, 1200 x 1600,
 * two controllers with one chip select each. Used in the Pimoroni Inky
 * Impression 13.3", the Waveshare 13.3inch e-Paper HAT+ (E) and the Good
 * Display GDEP133C02.
 *
 * UNTESTED ON HARDWARE. The command sequence is a port of two reference
 * drivers (credits in el133uf1.c) and has only been run against the panel
 * simulator in targets/wasm and the fake bus in tests/.
 *
 * The driver is portable C. It reaches the panel through the small bus
 * interface below, so the same code runs on the ESP32 (SPI and GPIO) and in
 * the browser emulator (where the bus is the panel simulator).
 */
#ifndef EL133UF1_H
#define EL133UF1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EL133UF1_CS0 0x01 /* first controller, left half (columns 0 to 599) */
#define EL133UF1_CS1 0x02 /* second controller, right half (columns 600 to 1199) */
#define EL133UF1_CS_BOTH 0x03

#define EL133UF1_HALF_BYTES 480000u
/* The panel maker asks for at least 180 seconds between two refreshes. */
#define EL133UF1_MIN_REFRESH_INTERVAL_MS 180000u

typedef struct {
    void *ctx;
    /* Drive the RESET line: 1 high (run), 0 low (reset). */
    void (*set_reset)(void *ctx, int level);
    /* Chip selects are active low. `mask` says which controllers are selected (pulled low). 0 releases both. */
    void (*set_cs)(void *ctx, unsigned mask);
    /* Data/command line: 0 command, 1 data. */
    void (*set_dc)(void *ctx, int level);
    /* Clock bytes out on MOSI, SPI mode 0, MSB first. */
    void (*write)(void *ctx, const uint8_t *data, size_t len);
    /* Raw level of the BUSY line. The line is active low: 0 means busy, 1 means ready. */
    int (*read_busy)(void *ctx);
    void (*delay_ms)(void *ctx, uint32_t ms);
} el133uf1_bus_t;

typedef enum {
    EL133UF1_IDLE = 0,   /* nothing in progress, panel power off */
    EL133UF1_LOADING,    /* begin_frame done, pixel data being written */
    EL133UF1_REFRESHING, /* refresh command sent */
} el133uf1_state_t;

typedef struct {
    el133uf1_bus_t bus;
    el133uf1_state_t state;
    int open_half;      /* half whose data transfer is open, or -1 */
    size_t written[2];  /* bytes written per half in this frame */
} el133uf1_t;

void el133uf1_init(el133uf1_t *dev, const el133uf1_bus_t *bus);

/* Hardware reset and the full register setup. Returns 0, or -1 if the panel stays busy. */
int el133uf1_begin_frame(el133uf1_t *dev);

/*
 * Append pixel bytes for one half (0 or 1). Each half takes exactly
 * EL133UF1_HALF_BYTES in rows of 300 bytes, top row first, two pixels per
 * byte with the left pixel in the high four bits. Finish half 0 before
 * starting half 1. Returns 0, or -1 on misuse or overflow.
 */
int el133uf1_write(el133uf1_t *dev, int half, const uint8_t *data, size_t len);

/*
 * Power on and start the refresh. Returns at once; poll el133uf1_busy.
 * Returns -1 if a refresh is already running or the frame is incomplete.
 */
int el133uf1_refresh(el133uf1_t *dev);

/* True while the panel is refreshing. */
bool el133uf1_busy(el133uf1_t *dev);

/* Power the panel off. Call after the refresh has ended, or to drop a partly loaded frame. */
void el133uf1_power_off(el133uf1_t *dev);

#ifdef __cplusplus
}
#endif

#endif
