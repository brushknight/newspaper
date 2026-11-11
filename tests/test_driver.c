/*
 * Host tests for the EL133UF1 driver against a fake bus that decodes the
 * command stream per controller. This checks the logic (which bytes go to
 * which chip select, command order, refusing misuse). It cannot check that
 * a real panel accepts the sequence.
 */
#include <stdio.h>
#include <string.h>

#include "el133uf1.h"

static int failures;

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                              \
        }                                                            \
    } while (0)

typedef struct {
    unsigned cs;
    int dc;
    int reset_pulses;
    int reset_level;
    uint8_t cmd[2];       /* current command per controller */
    size_t dtm_bytes[2];  /* pixel bytes received per controller */
    uint32_t dtm_sum[2];
    int dtm_commands[2];
    char order[64];       /* P = PON, R = DRF, O = POF, as seen by controller 0 */
    int busy_reads_left;  /* while > 0 the panel reports busy */
    bool refreshing;
    int init_commands;
    uint32_t delayed;
} bus_t;

static void b_reset(void *ctx, int level)
{
    bus_t *b = ctx;
    if (b->reset_level == 1 && level == 0) {
        b->reset_pulses++;
    }
    b->reset_level = level;
}

static void b_cs(void *ctx, unsigned mask)
{
    ((bus_t *)ctx)->cs = mask;
}

static void b_dc(void *ctx, int level)
{
    ((bus_t *)ctx)->dc = level;
}

static void b_write(void *ctx, const uint8_t *data, size_t len)
{
    bus_t *b = ctx;
    for (int c = 0; c < 2; c++) {
        if (!(b->cs & (1u << c))) {
            continue;
        }
        if (b->dc == 0) {
            b->cmd[c] = data[0];
            if (data[0] == 0x10) {
                b->dtm_commands[c]++;
            } else if (c == 0) {
                size_t n = strlen(b->order);
                if (data[0] == 0x04) b->order[n] = 'P';
                else if (data[0] == 0x12) b->order[n] = 'R';
                else if (data[0] == 0x02) b->order[n] = 'O';
                else b->init_commands++;
            }
            if (data[0] == 0x12) {
                b->refreshing = true;
                b->busy_reads_left = 5;
            }
        } else if (b->cmd[c] == 0x10) {
            b->dtm_bytes[c] += len;
            for (size_t i = 0; i < len; i++) {
                b->dtm_sum[c] += data[i];
            }
        }
    }
}

static int b_busy(void *ctx)
{
    bus_t *b = ctx;
    if (b->busy_reads_left > 0) {
        b->busy_reads_left--;
        return 0;
    }
    b->refreshing = false;
    return 1;
}

static void b_delay(void *ctx, uint32_t ms)
{
    ((bus_t *)ctx)->delayed += ms;
}

int main(void)
{
    bus_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.reset_level = 1;
    el133uf1_bus_t iface = { &bus, b_reset, b_cs, b_dc, b_write, b_busy, b_delay };
    el133uf1_t dev;
    el133uf1_init(&dev, &iface);

    /* Misuse before begin_frame is refused. */
    uint8_t chunk[3000];
    memset(chunk, 0x11, sizeof(chunk));
    CHECK(el133uf1_write(&dev, 0, chunk, sizeof(chunk)) != 0);
    CHECK(el133uf1_refresh(&dev) != 0);
    CHECK(!el133uf1_busy(&dev));

    CHECK(el133uf1_begin_frame(&dev) == 0);
    CHECK(bus.reset_pulses == 1);
    CHECK(bus.init_commands == 20); /* 21 init commands, one of them goes to controller 1 only */
    CHECK(bus.cs == 0);

    /* An incomplete frame cannot be refreshed. */
    CHECK(el133uf1_write(&dev, 0, chunk, sizeof(chunk)) == 0);
    CHECK(el133uf1_refresh(&dev) != 0);

    /* Half 0 is 0x11 bytes, half 1 is 0x33 bytes: the sums prove nothing crossed over. */
    for (size_t sent = sizeof(chunk); sent < EL133UF1_HALF_BYTES; sent += sizeof(chunk)) {
        CHECK(el133uf1_write(&dev, 0, chunk, sizeof(chunk)) == 0);
    }
    CHECK(el133uf1_write(&dev, 0, chunk, 1) != 0); /* overflow */
    memset(chunk, 0x33, sizeof(chunk));
    for (size_t sent = 0; sent < EL133UF1_HALF_BYTES; sent += sizeof(chunk)) {
        CHECK(el133uf1_write(&dev, 1, chunk, sizeof(chunk)) == 0);
    }
    CHECK(el133uf1_write(&dev, 0, chunk, 1) != 0); /* cannot go back to half 0 */
    CHECK(bus.dtm_bytes[0] == EL133UF1_HALF_BYTES);
    CHECK(bus.dtm_bytes[1] == EL133UF1_HALF_BYTES);
    CHECK(bus.dtm_sum[0] == EL133UF1_HALF_BYTES * 0x11u);
    CHECK(bus.dtm_sum[1] == EL133UF1_HALF_BYTES * 0x33u);
    CHECK(bus.dtm_commands[0] == 1 && bus.dtm_commands[1] == 1); /* one transfer per half */

    CHECK(el133uf1_refresh(&dev) == 0);
    CHECK(strcmp(bus.order, "PR") == 0);
    CHECK(bus.cs == 0);
    CHECK(el133uf1_refresh(&dev) != 0); /* already refreshing */
    bus.busy_reads_left = 3;
    CHECK(el133uf1_busy(&dev));
    el133uf1_power_off(&dev); /* refused while busy */
    CHECK(strcmp(bus.order, "PR") == 0);
    CHECK(el133uf1_begin_frame(&dev) != 0); /* refused while busy */
    bus.busy_reads_left = 0;
    CHECK(!el133uf1_busy(&dev));
    el133uf1_power_off(&dev);
    CHECK(strcmp(bus.order, "PRO") == 0);

    /* Dropping a partly loaded frame sends no power-off (power was never on). */
    CHECK(el133uf1_begin_frame(&dev) == 0);
    CHECK(el133uf1_write(&dev, 0, chunk, sizeof(chunk)) == 0);
    el133uf1_power_off(&dev);
    CHECK(strcmp(bus.order, "PRO") == 0);
    CHECK(bus.cs == 0);
    CHECK(el133uf1_refresh(&dev) != 0);

    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
