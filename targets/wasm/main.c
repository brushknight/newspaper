/*
 * Browser emulator target: the same core and the same EL133UF1 driver as
 * the firmware, compiled with Emscripten.
 *
 * The HAL below forwards to a JavaScript "host" object (web/emulator.js):
 *   - http            fetch, read chunk by chunk
 *   - storage         localStorage
 *   - buttons         a queue filled by the page
 *   - display         the real driver from drivers/el133uf1, whose bus
 *                     (reset, chip selects, D/C, bytes, BUSY) is wired to the
 *                     panel simulator in web/panel-sim.js. The simulator sees
 *                     exactly the bytes a real panel would.
 *
 * Blocking HAL calls are made possible by Asyncify: the C code simply waits.
 */
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "el133uf1.h"

#ifndef NP_FIRMWARE_VERSION
#define NP_FIRMWARE_VERSION "0.0.0-dev"
#endif

/* ---- JavaScript side ---- */

EM_JS(void, js_bus_reset, (int level), { Module.host.panel.busReset(level); });
EM_JS(void, js_bus_cs, (unsigned mask), { Module.host.panel.busCs(mask); });
EM_JS(void, js_bus_dc, (int level), { Module.host.panel.busDc(level); });
EM_JS(void, js_bus_write, (const uint8_t *data, size_t len), {
    Module.host.panel.busWrite(HEAPU8.subarray(data, data + len));
});
EM_JS(int, js_bus_busy, (void), { return Module.host.panel.busBusyLevel(); });
EM_JS(double, js_min_refresh_ms, (void), { return Module.host.panel.minRefreshIntervalMs(); });

EM_ASYNC_JS(void, js_sleep, (double ms), { await Module.host.sleep(ms); });
EM_ASYNC_JS(void, js_idle, (double ms), { await Module.host.idle(ms); });
EM_JS(double, js_now, (void), { return Date.now(); });

EM_ASYNC_JS(int, js_http_open, (const char *method, const char *url, const char *headers, const char *body), {
    return await Module.host.httpOpen(UTF8ToString(method), UTF8ToString(url), UTF8ToString(headers),
                                      body ? UTF8ToString(body) : null);
});
EM_JS(int, js_http_status, (int handle), { return Module.host.httpStatus(handle); });
EM_JS(int, js_http_retry_after, (int handle), { return Module.host.httpRetryAfter(handle); });
EM_ASYNC_JS(int, js_http_read, (int handle, uint8_t *buf, int cap), {
    const piece = await Module.host.httpRead(handle, cap);
    if (piece === null) {
        return -1;
    }
    HEAPU8.set(piece, buf);
    return piece.length;
});
EM_JS(void, js_http_close, (int handle), { Module.host.httpClose(handle); });

EM_JS(int, js_storage_get, (const char *key, char *buf, int cap), {
    const value = Module.host.storageGet(UTF8ToString(key));
    if (value === null || lengthBytesUTF8(value) + 1 > cap) {
        return 0;
    }
    stringToUTF8(value, buf, cap);
    return 1;
});
EM_JS(void, js_storage_set, (const char *key, const char *value), {
    Module.host.storageSet(UTF8ToString(key), value ? UTF8ToString(value) : null);
});
EM_JS(int, js_button_poll, (void), { return Module.host.buttonPoll(); });
EM_JS(int, js_wifi_has, (void), { return Module.host.wifiHasCredentials() ? 1 : 0; });
EM_JS(int, js_prov_done, (void), { return Module.host.provisioningDone() ? 1 : 0; });
EM_JS(void, js_prov_start, (void), { Module.host.provisioningStart(); });
EM_JS(void, js_log, (int level, const char *message), { Module.host.log(level, UTF8ToString(message)); });
EM_JS(void, js_config, (const char *key, char *buf, int cap), {
    stringToUTF8(String(Module.host.config[UTF8ToString(key)] || ""), buf, cap);
});
EM_JS(void, js_report, (int state, int refreshing, const char *user_code, const char *uri, const char *screen,
                        int page, int pages), {
    Module.host.report(state, !!refreshing, UTF8ToString(user_code), UTF8ToString(uri), UTF8ToString(screen), page,
                       pages);
});

/* ---- panel bus: the driver's pins, wired to the simulator ---- */

static void bus_reset(void *ctx, int level) { js_bus_reset(level); }
static void bus_cs(void *ctx, unsigned mask) { js_bus_cs(mask); }
static void bus_dc(void *ctx, int level) { js_bus_dc(level); }
static void bus_write(void *ctx, const uint8_t *data, size_t len) { js_bus_write(data, len); }
static int bus_busy(void *ctx) { return js_bus_busy(); }
static void bus_delay(void *ctx, uint32_t ms) { js_sleep(ms); }

static el133uf1_t panel;

/* ---- HAL ---- */

static int hal_begin(void *ctx) { return el133uf1_begin_frame(&panel); }
static int hal_write(void *ctx, int half, const uint8_t *data, size_t len)
{
    return el133uf1_write(&panel, half, data, len);
}
static int hal_refresh(void *ctx) { return el133uf1_refresh(&panel); }
static bool hal_busy(void *ctx) { return el133uf1_busy(&panel); }
static void hal_power_off(void *ctx) { el133uf1_power_off(&panel); }
static uint32_t hal_min_interval(void *ctx) { return (uint32_t)js_min_refresh_ms(); }

static int hal_http(void *ctx, const np_http_request_t *req, np_http_body_cb on_body, void *user,
                    np_http_response_t *resp)
{
    /* Headers travel as "Name: value\n" lines. */
    char headers[1024];
    size_t n = 0;
    headers[0] = 0;
    for (int i = 0; i < req->header_count; i++) {
        int w = snprintf(headers + n, sizeof(headers) - n, "%s: %s\n", req->headers[i].name, req->headers[i].value);
        if (w < 0 || (size_t)w >= sizeof(headers) - n) {
            return -1;
        }
        n += (size_t)w;
    }
    int handle = js_http_open(req->method, req->url, headers, req->body);
    if (handle < 0) {
        return -1;
    }
    resp->status = js_http_status(handle); /* set before the first body callback, as the core expects */
    resp->retry_after_s = js_http_retry_after(handle);
    static uint8_t buf[16384];
    int rc = 0;
    for (;;) {
        int got = js_http_read(handle, buf, (int)sizeof(buf));
        if (got < 0) {
            rc = -1;
            break;
        }
        if (got == 0) {
            break;
        }
        if (!on_body(user, buf, (size_t)got)) {
            rc = -1;
            break;
        }
    }
    js_http_close(handle);
    return rc;
}

static bool hal_storage_get(void *ctx, const char *key, char *buf, size_t cap)
{
    return js_storage_get(key, buf, (int)cap) != 0;
}

static bool hal_storage_set(void *ctx, const char *key, const char *value)
{
    js_storage_set(key, value);
    return true;
}

static uint64_t hal_now(void *ctx) { return (uint64_t)js_now(); }
static void hal_idle(void *ctx, uint32_t ms, bool may_power_down) { js_idle(ms); }
static np_button_t hal_button(void *ctx) { return (np_button_t)js_button_poll(); }
static bool hal_wifi_has(void *ctx) { return js_wifi_has() != 0; }
static bool hal_wifi_connect(void *ctx) { return true; }
static int hal_rssi(void *ctx) { return 0; }

static bool hal_prov_start(void *ctx, np_prov_info_t *info)
{
    /* There is no access point in a browser; the page shows a stand-in for the captive portal. */
    snprintf(info->ssid, sizeof(info->ssid), "Newspaper-EMU");
    info->password[0] = 0;
    snprintf(info->url, sizeof(info->url), "http://192.168.4.1");
    js_prov_start();
    return true;
}

static bool hal_prov_done(void *ctx) { return js_prov_done() != 0; }
static void hal_prov_stop(void *ctx) {}
static int hal_battery(void *ctx) { return -1; }
static void hal_log(void *ctx, np_log_level_t level, const char *message) { js_log((int)level, message); }

static np_app_t app;

int main(void)
{
    static char api_base[128];
    static char hardware_id[40];
    js_config("api", api_base, sizeof(api_base));
    js_config("hardwareId", hardware_id, sizeof(hardware_id));

    el133uf1_bus_t bus = { NULL, bus_reset, bus_cs, bus_dc, bus_write, bus_busy, bus_delay };
    el133uf1_init(&panel, &bus);

    np_hal_t hal = {
        .display_begin_frame = hal_begin,
        .display_write = hal_write,
        .display_refresh = hal_refresh,
        .display_busy = hal_busy,
        .display_power_off = hal_power_off,
        .display_min_refresh_interval_ms = hal_min_interval,
        .http_request = hal_http,
        .storage_get = hal_storage_get,
        .storage_set = hal_storage_set,
        .now_ms = hal_now,
        .idle = hal_idle,
        .button_poll = hal_button,
        .wifi_has_credentials = hal_wifi_has,
        .wifi_connect = hal_wifi_connect,
        .wifi_rssi = hal_rssi,
        .prov_start = hal_prov_start,
        .prov_done = hal_prov_done,
        .prov_stop = hal_prov_stop,
        .battery_percent = hal_battery,
        .log = hal_log,
    };
    np_app_config_t config = {
        .api_base = api_base,
        .hardware_id = hardware_id,
        .firmware = NP_FIRMWARE_VERSION,
        .emulator = true,
    };
    np_app_init(&app, &hal, &config);

    char line[200];
    snprintf(line, sizeof(line), "emulator firmware %s, platform %s", NP_FIRMWARE_VERSION, api_base);
    js_log(NP_LOG_INFO, line);

    for (;;) {
        uint32_t wait = np_app_step(&app);
        js_report((int)app.state, app.refreshing, app.user_code, app.verify_uri_complete, app.screen,
                  app.drawn_page, app.page_count);
        if (wait > 0) {
            js_idle(wait);
        }
    }
    return 0;
}
