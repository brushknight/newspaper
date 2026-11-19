/*
 * Linux side of the HAL (Raspberry Pi with an Inky Impression 13.3", or any
 * Linux or macOS host in no-hardware mode): the pieces main.c assembles into
 * np_hal_t.
 *
 * Portable pieces (config.c, store.c, png.c, netinfo.c, wifi_file.c) build on
 * any POSIX host and have host tests in tests/test_linux.c. The hardware
 * pieces (gpio.c, panel_spi.c) build only on Linux.
 */
#ifndef NP_LINUX_PLATFORM_H
#define NP_LINUX_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "el133uf1.h"
#include "hal.h"

#define NP_PATH_MAX 256
#define NP_WIFI_HELP_LINES 3

/* ---- config.c: settings from /etc/newspaper/newspaper.conf and the command line ---- */

typedef struct {
    char api[128];
    char data_dir[NP_PATH_MAX];    /* durable keys (token, what is on the panel), mode 0700 */
    char runtime_dir[NP_PATH_MAX]; /* "rt_" keys; survives restarts of the program, not reboots. "" keeps them in memory */
    char hardware_id[40];          /* "" derives one from the board serial number */
    char ca_file[NP_PATH_MAX];     /* "" uses libcurl's default, or a probed bundle in static builds */
    bool verbose;

    /* no-hardware mode: frames go to files instead of the panel */
    bool no_hardware;
    char frames_dir[NP_PATH_MAX];
    uint32_t sim_refresh_ms; /* how long the simulated refresh holds BUSY */
    uint32_t sim_rest_ms;    /* minimum time between refreshes; only no-hardware mode may lower it */

    /* panel on the Raspberry Pi header (BCM GPIO numbers, line offsets on the gpiochip) */
    char spi_device[64];
    uint32_t spi_hz;
    char gpiochip[64]; /* "" finds the header's chip by its label */
    int pin_cs0, pin_cs1, pin_dc, pin_reset, pin_busy;
    int button_next, button_back, button_home; /* -1: not used */
    uint32_t debounce_ms;

    /* network */
    bool network_check;      /* wait for a default route and a plausible clock before HTTP */
    uint32_t network_wait_s; /* how long one connect attempt waits for the network */
    char wifi_help[NP_WIFI_HELP_LINES][64];
} np_linux_config_t;

void np_config_defaults(np_linux_config_t *cfg);
/* Set one key. Returns 0, or -1 with a message in err. */
int np_config_set(np_linux_config_t *cfg, const char *key, const char *value, char *err, size_t err_cap);
/* Parse "key = value" lines; # starts a comment. Returns 0, or -1 with "line N: ..." in err. */
int np_config_parse(np_linux_config_t *cfg, const char *text, char *err, size_t err_cap);
/* Returns 0, 1 when the file does not exist, -1 on a read or parse error. */
int np_config_load_file(np_linux_config_t *cfg, const char *path, char *err, size_t err_cap);

/* ---- store.c: key/value file ---- */

#define NP_STORE_MAX_KEYS 48
#define NP_STORE_VALUE_MAX 384

typedef struct {
    char path[NP_PATH_MAX + 16]; /* "" keeps the store in memory only */
    bool durable;                /* fsync file and directory after each write */
    int count;
    struct {
        char key[16];
        char value[NP_STORE_VALUE_MAX];
    } e[NP_STORE_MAX_KEYS];
} np_store_t;

/*
 * Load the store from `path` (a missing file is an empty store). The file
 * is created with mode 0600 on the first write. Returns 0, or -1 when the
 * file exists but cannot be read.
 */
int np_store_open(np_store_t *s, const char *path, bool durable);
bool np_store_get(const np_store_t *s, const char *key, char *buf, size_t cap);
/* value NULL erases. Returns false when the key or value is too long, or the write failed. */
bool np_store_set(np_store_t *s, const char *key, const char *value);
bool np_store_clear(np_store_t *s);
/* Text form used in the file, exposed for tests. Returns bytes written, or -1 if cap is too small. */
long np_store_serialize(const np_store_t *s, char *buf, size_t cap);
void np_store_parse(np_store_t *s, const char *text);

/* ---- png.c ---- */

/* Encode an el133uf1 frame (960,000 bytes) as a 1200 x 1600 palette PNG. Returns malloc'd bytes or NULL. */
uint8_t *np_frame_to_png(const uint8_t *frame, size_t *out_len);
uint32_t np_crc32(const uint8_t *data, size_t len, uint32_t crc);

/* ---- netinfo.c: parsers for /proc files ---- */

/* True when /proc/net/route text has an up default route, or /proc/net/ipv6_route text has a default route. Either may be NULL. */
bool np_net_has_default_route(const char *route, const char *ipv6_route);
/* Signal level in dBm of the first interface in /proc/net/wireless text, or 0. */
int np_net_wireless_dbm(const char *wireless);

/* ---- wifi_file.c: newspaper-wifi.txt for the flashable image ---- */

typedef struct {
    char ssid[33];
    char password[65];
    char country[3];
    bool hidden;
} np_wifi_file_t;

/* Returns 0 with an SSID, 1 when no SSID is set (an untouched file), -1 with a message in err. */
int np_wifi_file_parse(const char *text, np_wifi_file_t *out, char *err, size_t err_cap);
/* wpa_supplicant.conf text for the network. Returns 0, or -1 with a message in err. */
int np_wifi_file_to_wpa(const np_wifi_file_t *w, char *buf, size_t cap, char *err, size_t err_cap);
/* WPA passphrase to PSK (PBKDF2-HMAC-SHA1, 4096 rounds, 32 bytes), as in IEEE 802.11i. */
void np_wpa_psk(const char *passphrase, const uint8_t *ssid, size_t ssid_len, uint8_t out[32]);

/* ---- main.c: shared runtime state ---- */

typedef struct np_linux np_linux_t;

/* True once SIGTERM or SIGINT arrived. */
bool np_stop_requested(void);
/* File descriptor that becomes readable when a stop is requested (for poll). */
int np_stop_fd(void);
void np_log(np_log_level_t level, const char *fmt, ...);

/* ---- net.c: libcurl and network readiness ---- */

int net_init(const np_linux_config_t *cfg);
int net_http_request(void *ctx, const np_http_request_t *req, np_http_body_cb on_body, void *user,
                     np_http_response_t *resp);
bool net_wait_ready(void *ctx);
int net_rssi(void *ctx);

/* ---- panel_sim.c: el133uf1 bus that decodes the driver's bytes into image files ---- */

int panel_sim_init(el133uf1_bus_t *bus, const np_linux_config_t *cfg);

/* ---- panel_spi.c and gpio.c: the real panel (Linux only) ---- */

int panel_spi_init(el133uf1_bus_t *bus, const np_linux_config_t *cfg);
void panel_spi_release(void);

/* ---- buttons.c ---- */

int buttons_init(const np_linux_config_t *cfg);
np_button_t buttons_poll(void *ctx);
/* Block until a press is queued, a stop is requested, or the time is over. */
void buttons_wait(uint32_t ms);
bool buttons_home_held(void);

#endif
