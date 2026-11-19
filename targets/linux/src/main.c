/*
 * The Morning Newspaper display, Linux program (Raspberry Pi with a
 * Pimoroni Inky Impression 13.3", or any Linux or macOS host in
 * no-hardware mode).
 *
 * This file only assembles the HAL and runs the portable core (core/app.c).
 * What the display does is decided there; how it reaches the hardware is in
 * the other files of this directory.
 *
 *   newspaper [--config FILE] [--key value ...]
 *   newspaper --no-hardware --api http://127.0.0.1:8787 --data-dir /tmp/np
 *
 * Every setting of the config file also works as an option (pin_busy is
 * --pin-busy). See targets/linux/newspaper.conf for the list.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "el133uf1.h"
#include "platform.h"

#ifndef NP_FIRMWARE_VERSION
#define NP_FIRMWARE_VERSION "0.0.0-dev"
#endif
#define DEFAULT_CONFIG "/etc/newspaper/newspaper.conf"

static np_linux_config_t cfg;
static el133uf1_t panel;
static np_app_t app;
static np_store_t durable;
static np_store_t runtime;
static volatile sig_atomic_t stop_flag;
static int stop_pipe[2] = { -1, -1 };

/* ---- log ---- */

void np_log(np_log_level_t level, const char *fmt, ...)
{
    if (level == NP_LOG_DEBUG && !cfg.verbose) {
        return;
    }
    static const char tags[] = "DIWE";
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%6ld.%03ld] %c %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000L, tags[level & 3], line);
}

static void log_line(void *ctx, np_log_level_t level, const char *message)
{
    np_log(level, "%s", message);
}

/* ---- stop requests (SIGTERM from systemd or init, Ctrl-C) ---- */

static void on_signal(int sig)
{
    stop_flag = 1;
    if (stop_pipe[1] >= 0) {
        ssize_t n = write(stop_pipe[1], "x", 1);
        (void)n;
    }
}

bool np_stop_requested(void)
{
    return stop_flag != 0;
}

int np_stop_fd(void)
{
    return stop_pipe[0];
}

/* ---- display: the portable driver on the SPI bus or on the simulator ---- */

static int display_begin(void *ctx) { return el133uf1_begin_frame(&panel); }
static int display_write(void *ctx, int half, const uint8_t *data, size_t len)
{
    return el133uf1_write(&panel, half, data, len);
}
static int display_refresh(void *ctx) { return el133uf1_refresh(&panel); }
static bool display_busy(void *ctx) { return el133uf1_busy(&panel); }
static void display_power_off(void *ctx) { el133uf1_power_off(&panel); }

static uint32_t display_min_interval(void *ctx)
{
    /* Only the simulated panel may rest for less than the panel maker's 180 s. */
    if (cfg.no_hardware) {
        return cfg.sim_rest_ms;
    }
    return EL133UF1_MIN_REFRESH_INTERVAL_MS;
}

/* ---- storage: durable keys in data_dir/state, "rt_" keys in runtime_dir/runtime ---- */

static bool storage_get(void *ctx, const char *key, char *buf, size_t cap)
{
    return np_store_get(strncmp(key, "rt_", 3) == 0 ? &runtime : &durable, key, buf, cap);
}

static bool storage_set(void *ctx, const char *key, const char *value)
{
    return np_store_set(strncmp(key, "rt_", 3) == 0 ? &runtime : &durable, key, value);
}

/* ---- clock and idle ---- */

/*
 * A clock that never goes backwards and keeps counting through a system
 * suspend. It starts at boot, so the "rt_" keys in /run (which a reboot
 * clears) always match it.
 */
static uint64_t now_ms(void *ctx)
{
    struct timespec ts;
#ifdef CLOCK_BOOTTIME
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Where the ESP32 deep-sleeps, Linux simply blocks until the time is over or a button is pressed. */
static void idle(void *ctx, uint32_t ms, bool may_power_down)
{
    buttons_wait(ms);
}

/* ---- Wi-Fi: owned by the system ---- */

static bool wifi_has_credentials(void *ctx) { return true; }
static bool prov_start(void *ctx, np_prov_info_t *info) { return false; }
static bool prov_done(void *ctx) { return false; }
static void prov_stop(void *ctx) {}
static int battery_percent(void *ctx) { return -1; }

/* ---- setup ---- */

static void read_first_line(const char *path, char *buf, size_t cap)
{
    buf[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
}

static void hex_only(char *s)
{
    size_t w = 0;
    for (size_t r = 0; s[r]; r++) {
        char c = s[r];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            s[w++] = c;
        } else if (c >= 'A' && c <= 'F') {
            s[w++] = (char)(c - 'A' + 'a');
        }
    }
    s[w] = 0;
}

/* A stable label like the ESP32's "esp32-<mac>": the board serial number on a Raspberry Pi. */
static void make_hardware_id(char *out, size_t cap)
{
    char serial[64];
    read_first_line("/sys/firmware/devicetree/base/serial-number", serial, sizeof(serial));
    hex_only(serial);
    if (!serial[0]) {
        char line[256];
        FILE *f = fopen("/proc/cpuinfo", "r");
        while (f && fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Serial", 6) == 0 && strchr(line, ':')) {
                snprintf(serial, sizeof(serial), "%s", strchr(line, ':') + 1);
                hex_only(serial);
            }
        }
        if (f) {
            fclose(f);
        }
    }
    const char *s = serial;
    while (*s == '0' && strlen(s) > 8) {
        s++; /* Raspberry Pi serials are zero padded to 16 digits */
    }
    if (*s && strspn(s, "0") != strlen(s)) {
        snprintf(out, cap, "rpi-%s", s);
        return;
    }
    char id[64];
    read_first_line("/etc/machine-id", id, sizeof(id));
    hex_only(id);
    if (strlen(id) >= 12) {
        id[12] = 0;
        snprintf(out, cap, "linux-%s", id);
        return;
    }
    /* macOS or a bare container: hash the host name, so the label stays the same between runs. */
    char host[128] = "host";
    gethostname(host, sizeof(host) - 1);
    uint32_t h = 0x811c9dc5u;
    for (const char *p = host; *p; p++) {
        h = (h ^ (uint8_t)*p) * 0x01000193u;
    }
    snprintf(out, cap, "linux-%08x", (unsigned)h);
}

static int make_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0 || errno == EEXIST) {
        return 0;
    }
    np_log(NP_LOG_ERROR, "cannot create %s: %s", path, strerror(errno));
    return -1;
}

static void usage(void)
{
    printf("The Morning Newspaper display %s\n\n"
           "usage: newspaper [--config FILE] [--SETTING VALUE ...]\n"
           "       newspaper --forget          erase the pairing and what the panel shows, then exit\n"
           "       newspaper --version\n\n"
           "Settings are read from %s (or --config FILE) and can be given as options,\n"
           "for example --api URL, --data-dir DIR, --no-hardware, --frames-dir DIR, --verbose.\n"
           "The full list is in targets/linux/newspaper.conf of the source.\n",
           NP_FIRMWARE_VERSION, DEFAULT_CONFIG);
}

static bool is_flag(const char *key)
{
    return strcmp(key, "no-hardware") == 0 || strcmp(key, "no_hardware") == 0 || strcmp(key, "verbose") == 0;
}

static int parse_args(int argc, char **argv, bool *forget)
{
    char err[256];
    const char *config_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            config_path = argv[i] + 9;
        }
    }
    int rc = np_config_load_file(&cfg, config_path ? config_path : DEFAULT_CONFIG, err, sizeof(err));
    if (rc < 0 || (rc == 1 && config_path)) {
        fprintf(stderr, "newspaper: %s\n", rc < 0 ? err : "config file not found");
        return -1;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            exit(0);
        }
        if (strcmp(a, "--version") == 0) {
            printf("%s\n", NP_FIRMWARE_VERSION);
            exit(0);
        }
        if (strcmp(a, "--forget") == 0) {
            *forget = true;
            continue;
        }
        if (strcmp(a, "--config") == 0) {
            i++;
            continue;
        }
        if (strncmp(a, "--config=", 9) == 0) {
            continue;
        }
        if (strncmp(a, "--", 2) != 0) {
            fprintf(stderr, "newspaper: unexpected argument %s (see --help)\n", a);
            return -1;
        }
        char key[64];
        const char *value;
        const char *eq = strchr(a, '=');
        if (eq) {
            snprintf(key, sizeof(key), "%.*s", (int)(eq - a - 2), a + 2);
            value = eq + 1;
        } else {
            snprintf(key, sizeof(key), "%s", a + 2);
            if (is_flag(key)) {
                value = "yes";
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                fprintf(stderr, "newspaper: %s needs a value\n", a);
                return -1;
            }
        }
        if (np_config_set(&cfg, key, value, err, sizeof(err)) != 0) {
            fprintf(stderr, "newspaper: %s\n", err);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool forget = false;
    np_config_defaults(&cfg);
    if (parse_args(argc, argv, &forget) != 0) {
        return 2;
    }
    if (!cfg.no_hardware) {
        cfg.sim_rest_ms = EL133UF1_MIN_REFRESH_INTERVAL_MS; /* never shorter on a real panel */
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    umask(077);

    if (make_dir(cfg.data_dir, 0700) != 0) {
        return 1;
    }
    char path[NP_PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/state", cfg.data_dir);
    if (np_store_open(&durable, path, true) != 0) {
        np_log(NP_LOG_ERROR, "cannot read %s: %s", path, strerror(errno));
        return 1;
    }
    if (forget) {
        np_store_clear(&durable);
        if (cfg.runtime_dir[0]) {
            snprintf(path, sizeof(path), "%s/runtime", cfg.runtime_dir);
            unlink(path);
        }
        printf("newspaper: pairing and saved state erased; the display pairs again on its next start\n");
        return 0;
    }
    path[0] = 0;
    if (cfg.runtime_dir[0] && make_dir(cfg.runtime_dir, 0700) == 0) {
        snprintf(path, sizeof(path), "%s/runtime", cfg.runtime_dir);
    }
    np_store_open(&runtime, path, false);

    if (pipe(stop_pipe) == 0) {
        fcntl(stop_pipe[0], F_SETFD, FD_CLOEXEC);
        fcntl(stop_pipe[1], F_SETFD, FD_CLOEXEC);
        fcntl(stop_pipe[1], F_SETFL, O_NONBLOCK);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    static char hardware_id[48];
    if (cfg.hardware_id[0]) {
        snprintf(hardware_id, sizeof(hardware_id), "%s", cfg.hardware_id);
    } else {
        make_hardware_id(hardware_id, sizeof(hardware_id));
    }
    np_log(NP_LOG_INFO, "The Morning Newspaper display, firmware %s (linux%s)", NP_FIRMWARE_VERSION,
           cfg.no_hardware ? ", no hardware" : "");

    el133uf1_bus_t bus;
    if (cfg.no_hardware) {
        if (!cfg.frames_dir[0]) {
            char dir[sizeof(cfg.data_dir) + 8];
            snprintf(dir, sizeof(dir), "%s/frames", cfg.data_dir);
            if (strlen(dir) >= sizeof(cfg.frames_dir)) {
                np_log(NP_LOG_ERROR, "data_dir is too long for the frames directory; set frames_dir");
                return 2;
            }
            memcpy(cfg.frames_dir, dir, strlen(dir) + 1);
        }
        if (panel_sim_init(&bus, &cfg) != 0) {
            return 1;
        }
    } else {
#ifdef NP_HARDWARE
        if (panel_spi_init(&bus, &cfg) != 0) {
            return 1;
        }
#else
        np_log(NP_LOG_ERROR, "this build has no panel support (it is not Linux); run with --no-hardware");
        return 2;
#endif
    }
    if (buttons_init(&cfg) != 0) {
        np_log(NP_LOG_WARN, "buttons: not available; the display still runs, without page turns by hand");
    }
    el133uf1_init(&panel, &bus);
    if (net_init(&cfg) != 0) {
        np_log(NP_LOG_ERROR, "net: cannot initialise libcurl");
        return 1;
    }

    /* Start over: hold Home while the program starts, for five seconds. Erases the pairing (Wi-Fi is the system's). */
    if (buttons_home_held()) {
        np_log(NP_LOG_WARN, "Home is held: keep holding for 5 s to erase the pairing");
        int held = 0;
        while (buttons_home_held() && held < 5000 && !np_stop_requested()) {
            struct timespec ts = { 0, 100000000L };
            nanosleep(&ts, NULL);
            held += 100;
        }
        if (held >= 5000) {
            np_store_clear(&durable);
            np_store_clear(&runtime);
            np_log(NP_LOG_WARN, "pairing erased");
        }
        while (buttons_poll(NULL) != NP_BTN_NONE) {
        }
    }

    np_hal_t hal = {
        .display_begin_frame = display_begin,
        .display_write = display_write,
        .display_refresh = display_refresh,
        .display_busy = display_busy,
        .display_power_off = display_power_off,
        .display_min_refresh_interval_ms = display_min_interval,
        .http_request = net_http_request,
        .storage_get = storage_get,
        .storage_set = storage_set,
        .now_ms = now_ms,
        .idle = idle,
        .button_poll = buttons_poll,
        .wifi_has_credentials = wifi_has_credentials,
        .wifi_connect = net_wait_ready,
        .wifi_rssi = net_rssi,
        .prov_start = prov_start,
        .prov_done = prov_done,
        .prov_stop = prov_stop,
        .battery_percent = battery_percent,
        .log = log_line,
    };
    np_app_config_t config = {
        .api_base = cfg.api,
        .hardware_id = hardware_id,
        .firmware = NP_FIRMWARE_VERSION,
        .emulator = cfg.no_hardware, /* frames go to files, not to a panel on a wall */
    };
    for (int i = 0; i < NP_WIFI_HELP_LINES; i++) {
        config.wifi_help[i] = cfg.wifi_help[i][0] ? cfg.wifi_help[i] : NULL;
    }
    np_app_init(&app, &hal, &config);
    np_log(NP_LOG_INFO, "%s, platform %s, data in %s", hardware_id, cfg.api, cfg.data_dir);

    /* A stop request waits for a running refresh: it is never interrupted. */
    while (!(np_stop_requested() && !app.refreshing)) {
        uint32_t ms = np_app_step(&app);
        if (np_stop_requested() && !app.refreshing) {
            break;
        }
        if (ms > 0) {
            if (np_stop_requested()) {
                struct timespec ts = { 0, (long)(ms < 250 ? ms : 250) * 1000000L };
                nanosleep(&ts, NULL); /* only the refresh is left to wait for */
            } else {
                idle(NULL, ms, app.may_sleep);
            }
        }
    }
    np_log(NP_LOG_INFO, "stopping");
#ifdef NP_HARDWARE
    if (!cfg.no_hardware) {
        panel_spi_release();
    }
#endif
    return 0;
}
