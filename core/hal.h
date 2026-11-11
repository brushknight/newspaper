/*
 * Hardware abstraction layer for the Morning Newspaper display core.
 *
 * The core (everything in core/) is portable C11. It includes no ESP-IDF and
 * no browser headers. Everything it needs from the outside world goes through
 * the function table below. There are three implementations in this repo:
 *
 *   targets/esp32/main/hal_*.c   real hardware
 *   targets/wasm/hal_wasm.c      browser emulator
 *   tests/fake_hal.c             host unit tests
 *
 * All calls are made from one thread. Calls may block (HTTP, Wi-Fi connect).
 */
#ifndef NP_HAL_H
#define NP_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- buttons ---- */

typedef enum {
    NP_BTN_NONE = 0,
    NP_BTN_NEXT,
    NP_BTN_BACK,
    NP_BTN_HOME
} np_button_t;

/* ---- http ---- */

typedef struct {
    const char *name;
    const char *value;
} np_http_header_t;

typedef struct {
    const char *method; /* "GET" or "POST" */
    const char *url;    /* absolute URL */
    const np_http_header_t *headers;
    int header_count;
    const char *body;   /* NUL terminated JSON for POST, or NULL */
} np_http_request_t;

typedef struct {
    int status;        /* HTTP status code */
    int retry_after_s; /* value of a Retry-After header in seconds, or -1 */
} np_http_response_t;

/* Receives the response body piece by piece. Return false to abort the transfer. */
typedef bool (*np_http_body_cb)(void *user, const uint8_t *data, size_t len);

/* ---- provisioning ---- */

typedef struct {
    char ssid[33];     /* name of the setup access point */
    char password[65]; /* empty string for an open access point */
    char url[64];      /* address of the setup page, for example http://192.168.4.1 */
} np_prov_info_t;

/* ---- log ---- */

typedef enum { NP_LOG_DEBUG = 0, NP_LOG_INFO, NP_LOG_WARN, NP_LOG_ERROR } np_log_level_t;

/* ---- the table ---- */

typedef struct np_hal {
    void *ctx; /* passed back as the first argument of every function */

    /*
     * Display. A frame is two halves of NP_FRAME_HALF_BYTES each (see frame.h).
     * Sequence: begin_frame, write(half 0 ...) until the half is complete,
     * write(half 1 ...), refresh. refresh only starts the refresh and returns;
     * the core then polls busy until it reports false and calls power_off.
     * All functions return 0 on success and a negative number on failure.
     * refresh must fail (and do nothing) while the panel is busy.
     */
    int (*display_begin_frame)(void *ctx);
    int (*display_write)(void *ctx, int half, const uint8_t *data, size_t len);
    int (*display_refresh)(void *ctx);
    bool (*display_busy)(void *ctx);
    void (*display_power_off)(void *ctx);
    /* The panel's minimum time between two refreshes, in milliseconds. */
    uint32_t (*display_min_refresh_interval_ms)(void *ctx);

    /*
     * HTTP. Blocking. Returns 0 when a response was received (any status),
     * negative on a transport error or when the body callback aborted.
     * The body must be delivered in pieces; the frame is 960 kB and the core
     * never asks for it as one buffer.
     */
    int (*http_request)(void *ctx, const np_http_request_t *req, np_http_body_cb on_body, void *user,
                        np_http_response_t *resp);

    /*
     * Key/value storage for short strings. Keys are at most 15 characters.
     * Keys that start with "rt_" are runtime state: they must survive sleep
     * (so the core can pick up after a deep sleep wake) but may be lost on
     * power loss. All other keys must survive power loss.
     * get returns false when the key is absent. set with value NULL erases.
     */
    bool (*storage_get)(void *ctx, const char *key, char *buf, size_t cap);
    bool (*storage_set)(void *ctx, const char *key, const char *value);

    /*
     * Clock. Milliseconds on a clock that never goes backwards while the
     * device has power, and that keeps counting during sleep.
     */
    uint64_t (*now_ms)(void *ctx);

    /*
     * Timer. Wait until `ms` milliseconds have passed or a button is pressed,
     * whichever is first. When `may_power_down` is true the target is free to
     * enter its deepest sleep and come back through a full restart instead of
     * returning (the core restores itself from storage).
     */
    void (*idle)(void *ctx, uint32_t ms, bool may_power_down);

    /* Buttons. Returns the next queued press, or NP_BTN_NONE. */
    np_button_t (*button_poll)(void *ctx);

    /* Wi-Fi and provisioning hooks. */
    bool (*wifi_has_credentials)(void *ctx);
    bool (*wifi_connect)(void *ctx); /* blocking; true when the network is usable */
    int (*wifi_rssi)(void *ctx);     /* dBm, or 0 when unknown */
    bool (*prov_start)(void *ctx, np_prov_info_t *info);
    bool (*prov_done)(void *ctx);    /* true once credentials have been saved */
    void (*prov_stop)(void *ctx);

    /* Battery level in percent, or -1 on mains power. */
    int (*battery_percent)(void *ctx);

    void (*log)(void *ctx, np_log_level_t level, const char *message);
} np_hal_t;

#ifdef __cplusplus
}
#endif

#endif
