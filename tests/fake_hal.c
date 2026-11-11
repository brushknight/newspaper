#include "fake_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

static const uint8_t INKS[6] = { NP_INK_BLACK, NP_INK_WHITE, NP_INK_YELLOW, NP_INK_RED, NP_INK_BLUE, NP_INK_GREEN };

uint8_t fake_frame_byte(int page, size_t offset)
{
    /* Differs per page, per row and per half, so a misplaced chunk is noticed. */
    size_t half = offset / NP_FRAME_HALF_BYTES;
    size_t row = (offset % NP_FRAME_HALF_BYTES) / NP_ROW_BYTES;
    size_t col = offset % NP_ROW_BYTES;
    uint8_t a = INKS[(row / 7 + (size_t)page + half * 3) % 6];
    uint8_t b = INKS[(col / 5 + (size_t)page) % 6];
    return (uint8_t)((a << 4) | b);
}

/* ---- display ---- */

static int d_begin(void *ctx)
{
    fake_t *f = ctx;
    if (f->panel_busy_flag && f->now < f->busy_until) {
        f->violations++;
        return -1;
    }
    f->begin_count++;
    f->half_bytes[0] = f->half_bytes[1] = 0;
    return 0;
}

static int d_write(void *ctx, int half, const uint8_t *data, size_t len)
{
    fake_t *f = ctx;
    if (half < 0 || half > 1 || f->half_bytes[half] + len > NP_FRAME_HALF_BYTES) {
        f->violations++;
        return -1;
    }
    if (half == 1 && f->half_bytes[0] != NP_FRAME_HALF_BYTES) {
        f->violations++; /* half 1 before half 0 is complete */
        return -1;
    }
    memcpy(f->loading + (size_t)half * NP_FRAME_HALF_BYTES + f->half_bytes[half], data, len);
    f->half_bytes[half] += len;
    f->write_calls++;
    if (len > f->max_write) {
        f->max_write = len;
    }
    return 0;
}

static int d_refresh(void *ctx)
{
    fake_t *f = ctx;
    if (f->panel_busy_flag && f->now < f->busy_until) {
        f->violations++;
        return -1;
    }
    if (f->half_bytes[0] != NP_FRAME_HALF_BYTES || f->half_bytes[1] != NP_FRAME_HALF_BYTES) {
        f->violations++;
        return -1;
    }
    if (f->refresh_count > 0 && f->now < f->busy_until + f->min_interval_ms) {
        f->violations++; /* sooner than the minimum interval after the last refresh ended */
    }
    if (f->refresh_count < FAKE_MAX_LOG) {
        f->refresh_started[f->refresh_count] = f->now;
    }
    f->refresh_count++;
    f->panel_busy_flag = true;
    f->busy_until = f->now + f->refresh_ms;
    memcpy(f->shown, f->loading, NP_FRAME_BYTES);
    return 0;
}

static bool d_busy(void *ctx)
{
    fake_t *f = ctx;
    return f->panel_busy_flag && f->now < f->busy_until;
}

static void d_power_off(void *ctx)
{
    fake_t *f = ctx;
    if (d_busy(ctx)) {
        f->violations++;
    }
    f->power_off_count++;
}

static uint32_t d_min_interval(void *ctx)
{
    return ((fake_t *)ctx)->min_interval_ms;
}

/* ---- storage ---- */

static bool s_get(void *ctx, const char *key, char *buf, size_t cap)
{
    fake_t *f = ctx;
    for (int i = 0; i < FAKE_MAX_KEYS; i++) {
        if (f->kv[i].used && strcmp(f->kv[i].key, key) == 0) {
            snprintf(buf, cap, "%s", f->kv[i].value);
            return true;
        }
    }
    return false;
}

static bool s_set(void *ctx, const char *key, const char *value)
{
    fake_t *f = ctx;
    int slot = -1;
    f->storage_writes++;
    for (int i = 0; i < FAKE_MAX_KEYS; i++) {
        if (f->kv[i].used && strcmp(f->kv[i].key, key) == 0) {
            slot = i;
            break;
        }
        if (!f->kv[i].used && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0 || strlen(key) > 15) {
        return false;
    }
    if (!value) {
        f->kv[slot].used = false;
        return true;
    }
    f->kv[slot].used = true;
    snprintf(f->kv[slot].key, sizeof(f->kv[slot].key), "%s", key);
    snprintf(f->kv[slot].value, sizeof(f->kv[slot].value), "%s", value);
    return true;
}

const char *fake_get(fake_t *f, const char *key)
{
    for (int i = 0; i < FAKE_MAX_KEYS; i++) {
        if (f->kv[i].used && strcmp(f->kv[i].key, key) == 0) {
            return f->kv[i].value;
        }
    }
    return NULL;
}

/* ---- platform model ---- */

static bool send_text(np_http_body_cb cb, void *user, const char *text)
{
    return cb(user, (const uint8_t *)text, strlen(text));
}

static const char *header(const np_http_request_t *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

static int h_request(void *ctx, const np_http_request_t *req, np_http_body_cb cb, void *user,
                     np_http_response_t *resp)
{
    fake_t *f = ctx;
    char text[1024];
    resp->retry_after_s = -1;
    if (f->net_down) {
        return -1;
    }
    const char *path = strstr(req->url, "/api/");
    if (!path) {
        resp->status = 404;
        return 0;
    }
    if (req->body) {
        snprintf(f->last_body, sizeof(f->last_body), "%s", req->body);
    }

    if (strcmp(path, "/api/v1/devices/pair/start") == 0) {
        f->start_calls++;
        f->pair_serial++;
        f->adopted = false;
        f->token_given = false;
        f->pair_expires_at = f->now + (uint64_t)f->pair_expires_in * 1000u;
        f->saw_emulator_flag = strstr(req->body, "\"emulator\":true") != NULL;
        resp->status = 200;
        snprintf(text, sizeof(text),
                 "{\"device_code\":\"devcode-%d-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-\","
                 "\"user_code\":\"K7QF-3MX%c\","
                 "\"verification_uri\":\"https://newspaper.brushknight.com/app/pair\","
                 "\"verification_uri_complete\":\"https://newspaper.brushknight.com/app/pair?code=K7QF-3MX%c\","
                 "\"expires_in\":%d,\"interval\":%d}",
                 f->pair_serial, 'A' + f->pair_serial, 'A' + f->pair_serial, f->pair_expires_in, f->pair_interval);
        send_text(cb, user, text);
        return 0;
    }
    if (strcmp(path, "/api/v1/devices/pair/poll") == 0) {
        if (f->poll_calls > 0) {
            uint64_t gap = f->now - f->last_poll_at;
            if (gap < f->min_poll_gap) {
                f->min_poll_gap = gap;
            }
        }
        f->poll_calls++;
        f->last_poll_at = f->now;
        char expect[64];
        snprintf(expect, sizeof(expect), "devcode-%d-", f->pair_serial);
        if (!strstr(req->body, expect)) {
            resp->status = 404;
            send_text(cb, user, "{\"error\":\"unknown\"}");
        } else if (f->token_given || f->now >= f->pair_expires_at) {
            resp->status = 410;
            send_text(cb, user, "{\"status\":\"expired\"}");
        } else if (f->adopted) {
            f->token_given = true;
            f->token_valid = true;
            resp->status = 200;
            send_text(cb, user,
                      "{\"status\":\"adopted\",\"device_id\":\"d-1\",\"name\":\"Kitchen\","
                      "\"token\":\"newspaper_dev_testtoken\"}");
        } else {
            resp->status = 200;
            send_text(cb, user, "{ \"status\": \"pending\" }");
        }
        return 0;
    }

    /* device API */
    const char *auth = header(req, "Authorization");
    f->saw_auth_header = auth != NULL;
    bool authed = f->token_valid && auth && strcmp(auth, "Bearer newspaper_dev_testtoken") == 0;
    if (strcmp(path, "/api/v1/device/state") == 0) {
        f->state_calls++;
        if (!authed) {
            resp->status = 401;
            send_text(cb, user, "{\"error\":\"unauthorized\"}");
            return 0;
        }
        resp->status = 200;
        int n = snprintf(text, sizeof(text),
                         "{\n  \"device\": { \"id\": \"d-1\", \"name\": \"Kitchen \\\"wall\\\"\", \"model\": \"%s\" },\n"
                         "  \"server_time\": \"2026-11-11T06:40:00Z\",\n"
                         "  \"config\": { \"check_interval_minutes\": %d, \"page_interval_minutes\": %d, "
                         "\"timezone\": \"Europe/Madrid\", \"quiet_hours\": null },\n  \"issue\": ",
                         NP_MODEL_ID, f->check_interval_min, f->page_interval_min);
        if (!f->issue_present) {
            snprintf(text + n, sizeof(text) - (size_t)n, "null }");
        } else {
            n += snprintf(text + n, sizeof(text) - (size_t)n,
                          "{ \"date\": \"%s\", \"number\": 12, \"version\": \"%s\", \"pages\": [", f->issue_date,
                          f->issue_version);
            for (int i = 1; i <= f->issue_pages; i++) {
                n += snprintf(text + n, sizeof(text) - (size_t)n,
                              "%s{ \"index\": %d, \"id\": \"p%d\", \"title\": \"Page [%d], {x}\" }", i > 1 ? ", " : "",
                              i, i, i);
            }
            snprintf(text + n, sizeof(text) - (size_t)n, "] } }");
        }
        /* Deliver in two pieces to exercise body collection. */
        size_t len = strlen(text);
        cb(user, (const uint8_t *)text, len / 2);
        cb(user, (const uint8_t *)text + len / 2, len - len / 2);
        return 0;
    }
    if (strncmp(path, "/api/v1/device/frame?page=", 26) == 0) {
        int page = atoi(path + 26);
        if (f->frame_calls < FAKE_MAX_LOG) {
            f->frame_pages[f->frame_calls] = page;
        }
        f->frame_calls++;
        if (!authed) {
            resp->status = 401;
            send_text(cb, user, "{\"error\":\"unauthorized\"}");
            return 0;
        }
        if (!f->issue_present || page < 1 || page > f->issue_pages) {
            resp->status = 404;
            send_text(cb, user, "{\"error\":\"no such page\"}");
            return 0;
        }
        if (f->frame_503_left > 0) {
            f->frame_503_left--;
            resp->status = 503;
            resp->retry_after_s = 7;
            send_text(cb, user, "{\"error\":\"rendering\"}");
            return 0;
        }
        resp->status = 200;
        size_t total = f->frame_truncate ? f->frame_truncate : NP_FRAME_BYTES;
        static uint8_t chunk[20000];
        size_t sent = 0;
        int ci = 0;
        while (sent < total) {
            size_t want = f->chunk_sizes[ci] ? f->chunk_sizes[ci] : 4096;
            ci = (ci + 1) % 4;
            if (f->chunk_sizes[ci] == 0) {
                ci = 0;
            }
            if (want > sizeof(chunk)) {
                want = sizeof(chunk);
            }
            if (want > total - sent) {
                want = total - sent;
            }
            for (size_t i = 0; i < want; i++) {
                chunk[i] = fake_frame_byte(page, sent + i);
            }
            if (!cb(user, chunk, want)) {
                return -1;
            }
            sent += want;
        }
        return 0;
    }
    resp->status = 404;
    return 0;
}

/* ---- the rest ---- */

static uint64_t c_now(void *ctx)
{
    return ((fake_t *)ctx)->now;
}

static void c_idle(void *ctx, uint32_t ms, bool may_power_down)
{
    (void)may_power_down;
    ((fake_t *)ctx)->now += ms;
}

static np_button_t b_poll(void *ctx)
{
    fake_t *f = ctx;
    if (f->queue_len == 0) {
        return NP_BTN_NONE;
    }
    np_button_t b = f->queue[0];
    memmove(f->queue, f->queue + 1, sizeof(f->queue[0]) * (size_t)(f->queue_len - 1));
    f->queue_len--;
    return b;
}

void fake_press(fake_t *f, np_button_t b)
{
    if (f->queue_len < 16) {
        f->queue[f->queue_len++] = b;
    }
}

static bool w_has(void *ctx)
{
    return ((fake_t *)ctx)->wifi_creds;
}

static bool w_connect(void *ctx)
{
    return ((fake_t *)ctx)->wifi_ok;
}

static int w_rssi(void *ctx)
{
    (void)ctx;
    return -61;
}

static bool p_start(void *ctx, np_prov_info_t *info)
{
    fake_t *f = ctx;
    f->prov_started = true;
    f->prov_start_calls++;
    snprintf(info->ssid, sizeof(info->ssid), "Newspaper-TEST");
    info->password[0] = 0;
    snprintf(info->url, sizeof(info->url), "http://192.168.4.1");
    return true;
}

static bool p_done(void *ctx)
{
    return ((fake_t *)ctx)->prov_finished;
}

static void p_stop(void *ctx)
{
    fake_t *f = ctx;
    f->prov_started = false;
    f->wifi_creds = true;
}

static int bat(void *ctx)
{
    (void)ctx;
    return -1;
}

static void log_line(void *ctx, np_log_level_t level, const char *message)
{
    fake_t *f = ctx;
    if (getenv("NP_TEST_VERBOSE")) {
        printf("    [%8llu ms] %d %s\n", (unsigned long long)f->now, (int)level, message);
    }
}

void fake_init(fake_t *f)
{
    memset(f, 0, sizeof(*f));
    f->loading = calloc(1, NP_FRAME_BYTES);
    f->shown = calloc(1, NP_FRAME_BYTES);
    f->now = 1000;
    f->refresh_ms = 15000;
    f->min_interval_ms = 180000;
    f->wifi_creds = true;
    f->wifi_ok = true;
    f->pair_expires_in = 900;
    f->pair_interval = 5;
    f->min_poll_gap = UINT64_MAX;
    f->issue_present = true;
    snprintf(f->issue_version, sizeof(f->issue_version), "v1");
    snprintf(f->issue_date, sizeof(f->issue_date), "2026-11-11");
    f->issue_pages = 3;
    f->check_interval_min = 30;
    f->page_interval_min = 0;
}

void fake_free(fake_t *f)
{
    free(f->loading);
    free(f->shown);
}

np_hal_t fake_hal(fake_t *f)
{
    np_hal_t hal = {
        .ctx = f,
        .display_begin_frame = d_begin,
        .display_write = d_write,
        .display_refresh = d_refresh,
        .display_busy = d_busy,
        .display_power_off = d_power_off,
        .display_min_refresh_interval_ms = d_min_interval,
        .http_request = h_request,
        .storage_get = s_get,
        .storage_set = s_set,
        .now_ms = c_now,
        .idle = c_idle,
        .button_poll = b_poll,
        .wifi_has_credentials = w_has,
        .wifi_connect = w_connect,
        .wifi_rssi = w_rssi,
        .prov_start = p_start,
        .prov_done = p_done,
        .prov_stop = p_stop,
        .battery_percent = bat,
        .log = log_line,
    };
    return hal;
}
