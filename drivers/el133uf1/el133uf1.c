/*
 * EL133UF1 (13.3 inch Spectra 6) panel driver.
 *
 * UNTESTED ON HARDWARE. Written before the panel arrived. Treat every
 * register value and timing below as "copied from a reference", not as
 * "measured on a device".
 *
 * Ported from, with thanks:
 *
 *   Pimoroni Inky library, inky/inky_el133uf1.py
 *   https://github.com/pimoroni/inky
 *   Copyright (c) 2018 Pimoroni Ltd, MIT License
 *   Source of: the register names, the init sequence used here, the split
 *   of commands between the two chip selects, the BUSY polarity, and the
 *   "wait for BUSY to assert before waiting for it to clear" rule.
 *
 *   Waveshare e-Paper library, epd13in3E
 *   https://github.com/waveshareteam/e-Paper
 *   Copyright (c) Waveshare team, MIT style permission notice
 *   Cross check for the sequence PON, DRF, POF, for holding a chip select
 *   low across the whole data transfer, and for the native orientation:
 *   1200 x 1600 portrait, left 600 columns to the first controller and the
 *   right 600 columns to the second, 300 bytes per row.
 *
 * Differences between the two references are noted where they matter.
 * The native orientation equals the el133uf1 frame format in
 * docs/PROTOCOL.md, so this driver needs no rotation or mirroring.
 */
#include "el133uf1.h"

/* Register names as in the Pimoroni driver. */
#define REG_PSR 0x00
#define REG_PWR 0x01
#define REG_POF 0x02
#define REG_POFS 0x03
#define REG_PON 0x04
#define REG_BTST_N 0x05
#define REG_BTST_P 0x06
#define REG_DTM 0x10
#define REG_DRF 0x12
#define REG_PLL 0x30
#define REG_CDI 0x50
#define REG_TCON 0x60
#define REG_TRES 0x61
#define REG_ANTM 0x74
#define REG_AGID 0x86
#define REG_BUCK_BOOST_VDDN 0xB0
#define REG_TFT_VCOM_POWER 0xB1
#define REG_EN_BUF 0xB6
#define REG_BOOST_VDDP_EN 0xB7
#define REG_CMDA4 0xA4
#define REG_DCDC 0xA5
#define REG_CCSET 0xE0
#define REG_PWS 0xE3
#define REG_CMD66 0xF0

/* PLL value 0x08 is Pimoroni's "normal" speed (dynamic frame rate). */
#define PLL_NORMAL 0x08

static void command(el133uf1_t *dev, unsigned cs, uint8_t reg, const uint8_t *data, size_t len)
{
    const el133uf1_bus_t *b = &dev->bus;
    b->set_cs(b->ctx, cs);
    b->set_dc(b->ctx, 0);
    b->write(b->ctx, &reg, 1);
    if (len > 0) {
        b->set_dc(b->ctx, 1);
        b->write(b->ctx, data, len);
    }
    b->set_cs(b->ctx, 0);
    b->set_dc(b->ctx, 0);
}

#define CMD(cs, reg, ...)                                   \
    do {                                                    \
        static const uint8_t d_[] = { __VA_ARGS__ };        \
        command(dev, (cs), (reg), d_, sizeof(d_));          \
    } while (0)

/* Wait until BUSY reads ready (high). Returns false on timeout. */
static bool wait_ready(el133uf1_t *dev, uint32_t timeout_ms)
{
    const el133uf1_bus_t *b = &dev->bus;
    uint32_t waited = 0;
    while (b->read_busy(b->ctx) == 0) {
        if (waited >= timeout_ms) {
            return false;
        }
        b->delay_ms(b->ctx, 10);
        waited += 10;
    }
    return true;
}

static void close_data(el133uf1_t *dev)
{
    if (dev->open_half >= 0) {
        dev->bus.set_cs(dev->bus.ctx, 0);
        dev->bus.set_dc(dev->bus.ctx, 0);
        dev->open_half = -1;
    }
}

void el133uf1_init(el133uf1_t *dev, const el133uf1_bus_t *bus)
{
    dev->bus = *bus;
    dev->state = EL133UF1_IDLE;
    dev->open_half = -1;
    dev->written[0] = dev->written[1] = 0;
}

int el133uf1_begin_frame(el133uf1_t *dev)
{
    const el133uf1_bus_t *b = &dev->bus;
    if (dev->state == EL133UF1_REFRESHING && b->read_busy(b->ctx) == 0) {
        return -1; /* never touch the panel during a refresh */
    }
    close_data(dev);
    dev->written[0] = dev->written[1] = 0;

    /* Reset pulse as in the Pimoroni driver: 30 ms low, then 300 ms to settle. */
    b->set_cs(b->ctx, 0);
    b->set_dc(b->ctx, 0);
    b->set_reset(b->ctx, 0);
    b->delay_ms(b->ctx, 30);
    b->set_reset(b->ctx, 1);
    b->delay_ms(b->ctx, 300);
    if (!wait_ready(dev, 1000)) {
        return -1;
    }

    /*
     * Init sequence from the Pimoroni driver. Waveshare's differs in a few
     * values (ANTM C0 1C 1C CC CC CC 15 15 55, PSR DF 69, CDI F7, booster
     * E8 28) and leaves out DCDC, PLL, POFS and CMDA4. Both are shipped by
     * their vendors for the same panel; Pimoroni's is the newer one.
     */
    CMD(EL133UF1_CS0, REG_ANTM, 0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55);
    CMD(EL133UF1_CS_BOTH, REG_CMD66, 0x49, 0x55, 0x13, 0x5D, 0x05, 0x10);
    CMD(EL133UF1_CS_BOTH, REG_PSR, 0xDF, 0x6B);
    CMD(EL133UF1_CS0, REG_DCDC, 0x44, 0x54, 0x00);
    CMD(EL133UF1_CS_BOTH, REG_PLL, PLL_NORMAL);
    CMD(EL133UF1_CS_BOTH, REG_CDI, 0x37);
    CMD(EL133UF1_CS_BOTH, REG_TCON, 0x03, 0x03);
    CMD(EL133UF1_CS0, REG_POFS, 0x00, 0xC0, 0x03, 0xA8);
    CMD(EL133UF1_CS1, REG_POFS, 0x00, 0xC0, 0x03, 0x9A);
    CMD(EL133UF1_CS_BOTH, REG_AGID, 0x10);
    CMD(EL133UF1_CS_BOTH, REG_PWS, 0x22);
    CMD(EL133UF1_CS_BOTH, REG_CCSET, 0x01);
    CMD(EL133UF1_CS_BOTH, REG_TRES, 0x04, 0xB0, 0x03, 0x20);
    CMD(EL133UF1_CS0, REG_CMDA4, 0x03, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00);
    CMD(EL133UF1_CS0, REG_PWR, 0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38);
    CMD(EL133UF1_CS0, REG_EN_BUF, 0x07);
    CMD(EL133UF1_CS0, REG_BTST_P, 0xE0, 0x20);
    CMD(EL133UF1_CS0, REG_BOOST_VDDP_EN, 0x01);
    CMD(EL133UF1_CS0, REG_BTST_N, 0xE0, 0x20);
    CMD(EL133UF1_CS0, REG_BUCK_BOOST_VDDN, 0x01);
    CMD(EL133UF1_CS0, REG_TFT_VCOM_POWER, 0x02);

    dev->state = EL133UF1_LOADING;
    return 0;
}

int el133uf1_write(el133uf1_t *dev, int half, const uint8_t *data, size_t len)
{
    const el133uf1_bus_t *b = &dev->bus;
    if (dev->state != EL133UF1_LOADING || half < 0 || half > 1) {
        return -1;
    }
    if (dev->written[half] + len > EL133UF1_HALF_BYTES) {
        return -1;
    }
    if (dev->open_half != half) {
        if (dev->written[half] != 0) {
            return -1; /* a half cannot be resumed once its transfer was closed */
        }
        close_data(dev);
        /* DTM, then keep the chip select low for the whole half, as Waveshare does. */
        static const uint8_t dtm = REG_DTM;
        b->set_cs(b->ctx, half == 0 ? EL133UF1_CS0 : EL133UF1_CS1);
        b->set_dc(b->ctx, 0);
        b->write(b->ctx, &dtm, 1);
        b->set_dc(b->ctx, 1);
        dev->open_half = half;
    }
    if (len > 0) {
        b->write(b->ctx, data, len);
        dev->written[half] += len;
    }
    return 0;
}

int el133uf1_refresh(el133uf1_t *dev)
{
    const el133uf1_bus_t *b = &dev->bus;
    if (dev->state != EL133UF1_LOADING) {
        return -1;
    }
    if (dev->written[0] != EL133UF1_HALF_BYTES || dev->written[1] != EL133UF1_HALF_BYTES) {
        return -1;
    }
    close_data(dev);

    command(dev, EL133UF1_CS_BOTH, REG_PON, NULL, 0);
    wait_ready(dev, 1000);
    b->delay_ms(b->ctx, 50); /* Waveshare waits 50 ms between PON and DRF */
    CMD(EL133UF1_CS_BOTH, REG_DRF, 0x00);

    /*
     * BUSY does not drop at once after DRF. Wait for it to assert (go low)
     * so that el133uf1_busy does not report "done" too early. Sending POF
     * in the middle of a refresh can latch the panel into a fault that
     * only removing power clears (note from the Pimoroni driver).
     */
    for (int i = 0; i < 200 && b->read_busy(b->ctx) != 0; i++) {
        b->delay_ms(b->ctx, 10);
    }
    dev->state = EL133UF1_REFRESHING;
    return 0;
}

bool el133uf1_busy(el133uf1_t *dev)
{
    if (dev->state != EL133UF1_REFRESHING) {
        return false;
    }
    return dev->bus.read_busy(dev->bus.ctx) == 0;
}

void el133uf1_power_off(el133uf1_t *dev)
{
    if (dev->state == EL133UF1_IDLE) {
        return;
    }
    if (dev->state == EL133UF1_REFRESHING && dev->bus.read_busy(dev->bus.ctx) == 0) {
        return; /* refuse: a refresh must not be interrupted */
    }
    close_data(dev);
    if (dev->state == EL133UF1_LOADING) {
        /* Dropping a partly loaded frame: PON was never sent, so there is nothing to power off. */
        dev->state = EL133UF1_IDLE;
        dev->written[0] = dev->written[1] = 0;
        return;
    }
    CMD(EL133UF1_CS_BOTH, REG_POF, 0x00);
    wait_ready(dev, 1000);
    dev->state = EL133UF1_IDLE;
    dev->written[0] = dev->written[1] = 0;
}
