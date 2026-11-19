/*
 * No-hardware mode: an el133uf1 bus that decodes the driver's command and
 * data bytes the way the panel would, and writes each refreshed frame to
 * files instead of ink. The real driver (drivers/el133uf1) runs on top of
 * it unchanged, as in the browser emulator.
 *
 * Per refresh it writes, in frames_dir:
 *   frame-NNNN.el133uf1   the raw 960,000 byte frame (docs/PROTOCOL.md section 4)
 *   frame-NNNN.png        the same as a 1200 x 1600 picture
 *   latest.png            a copy of the newest picture
 *
 * It also checks the panel's rules and logs an error for a refresh that
 * starts while BUSY is held or sooner than the rest period after the last.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "frame.h"
#include "platform.h"

#define CMD_PON 0x04
#define CMD_POF 0x02
#define CMD_DTM 0x10
#define CMD_DRF 0x12

typedef struct {
    const np_linux_config_t *cfg;
    unsigned cs;
    int dc;
    int reset;
    uint8_t cmd;
    uint8_t *frame; /* both halves, as in the frame format */
    size_t got[2];
    uint64_t busy_until;
    uint64_t last_refresh_end;
    bool has_refreshed;
    int count;
    int violations;
} sim_t;

static sim_t sim;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool write_file(const char *path, const uint8_t *data, size_t len)
{
    char tmp[NP_PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    bool ok = write(fd, data, len) == (ssize_t)len;
    ok = close(fd) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

static void refresh(void)
{
    uint64_t now = mono_ms();
    if (now < sim.busy_until) {
        sim.violations++;
        np_log(NP_LOG_ERROR, "panel-sim: RULE VIOLATION: refresh started while the panel is busy");
    } else if (sim.has_refreshed && now - sim.last_refresh_end < sim.cfg->sim_rest_ms) {
        sim.violations++;
        np_log(NP_LOG_ERROR, "panel-sim: RULE VIOLATION: refresh %u ms after the last one, minimum %u ms",
               (unsigned)(now - sim.last_refresh_end), (unsigned)sim.cfg->sim_rest_ms);
    }
    if (sim.got[0] != NP_FRAME_HALF_BYTES || sim.got[1] != NP_FRAME_HALF_BYTES) {
        np_log(NP_LOG_WARN, "panel-sim: refresh with %u + %u bytes loaded, expected %u each", (unsigned)sim.got[0],
               (unsigned)sim.got[1], (unsigned)NP_FRAME_HALF_BYTES);
    }
    sim.count++;
    sim.busy_until = now + sim.cfg->sim_refresh_ms;
    sim.last_refresh_end = sim.busy_until;
    sim.has_refreshed = true;

    const char *dir = sim.cfg->frames_dir;
    char path[NP_PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/frame-%04d.el133uf1", dir, sim.count);
    bool ok = write_file(path, sim.frame, NP_FRAME_BYTES);
    size_t png_len = 0;
    uint8_t *png = np_frame_to_png(sim.frame, &png_len);
    if (png) {
        snprintf(path, sizeof(path), "%s/frame-%04d.png", dir, sim.count);
        ok = write_file(path, png, png_len) && ok;
        snprintf(path, sizeof(path), "%s/latest.png", dir);
        ok = write_file(path, png, png_len) && ok;
        free(png);
    }
    if (ok) {
        np_log(NP_LOG_INFO, "panel-sim: refresh %d written to %s/frame-%04d.png", sim.count, dir, sim.count);
    } else {
        np_log(NP_LOG_ERROR, "panel-sim: cannot write frame %d to %s: %s", sim.count, dir, strerror(errno));
    }
}

static void bus_set_reset(void *ctx, int level)
{
    if (sim.reset && !level) {
        sim.cmd = 0; /* a reset pulse starts a new frame */
    }
    sim.reset = level;
}

static void bus_set_cs(void *ctx, unsigned mask)
{
    sim.cs = mask;
}

static void bus_set_dc(void *ctx, int level)
{
    sim.dc = level;
}

static void bus_write(void *ctx, const uint8_t *data, size_t len)
{
    if (!sim.cs) {
        return; /* nobody listens */
    }
    if (sim.dc == 0) {
        for (size_t i = 0; i < len; i++) {
            sim.cmd = data[i];
            if (sim.cmd == CMD_DTM) {
                if (sim.cs & EL133UF1_CS0) {
                    sim.got[0] = 0;
                }
                if (sim.cs & EL133UF1_CS1) {
                    sim.got[1] = 0;
                }
            } else if (sim.cmd == CMD_DRF) {
                refresh();
            }
        }
        return;
    }
    if (sim.cmd != CMD_DTM) {
        return; /* register data; the simulation does not model it */
    }
    for (int half = 0; half < 2; half++) {
        if (!(sim.cs & (half == 0 ? EL133UF1_CS0 : EL133UF1_CS1))) {
            continue;
        }
        size_t room = NP_FRAME_HALF_BYTES - sim.got[half];
        size_t n = len < room ? len : room;
        memcpy(sim.frame + (size_t)half * NP_FRAME_HALF_BYTES + sim.got[half], data, n);
        sim.got[half] += n;
    }
}

static int bus_read_busy(void *ctx)
{
    return mono_ms() < sim.busy_until ? 0 : 1;
}

static void bus_delay(void *ctx, uint32_t ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

int panel_sim_init(el133uf1_bus_t *bus, const np_linux_config_t *cfg)
{
    memset(&sim, 0, sizeof(sim));
    sim.cfg = cfg;
    sim.reset = 1;
    sim.frame = calloc(1, NP_FRAME_BYTES);
    if (!sim.frame) {
        return -1;
    }
    if (mkdir(cfg->frames_dir, 0755) != 0 && errno != EEXIST) {
        np_log(NP_LOG_ERROR, "panel-sim: cannot create %s: %s", cfg->frames_dir, strerror(errno));
        return -1;
    }
    bus->ctx = NULL;
    bus->set_reset = bus_set_reset;
    bus->set_cs = bus_set_cs;
    bus->set_dc = bus_set_dc;
    bus->write = bus_write;
    bus->read_busy = bus_read_busy;
    bus->delay_ms = bus_delay;
    np_log(NP_LOG_INFO, "panel-sim: no hardware; frames go to %s (refresh %u ms, rest %u ms)", cfg->frames_dir,
           (unsigned)cfg->sim_refresh_ms, (unsigned)cfg->sim_rest_ms);
    return 0;
}
