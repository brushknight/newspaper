#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"
#include "json.h"
#include "screen.h"

#define MINUTE_MS 60000ull
#define DEFAULT_CHECK_MIN 30u
#define DEFAULT_PAGE_MIN 10u
#define BUSY_POLL_MS 250u
#define MAX_WAIT_MS (24u * 60u * 60u * 1000u)

/* ---------- small helpers ---------- */

static void logf_(np_app_t *app, np_log_level_t level, const char *fmt, ...)
{
    if (!app->hal.log) {
        return;
    }
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    app->hal.log(app->hal.ctx, level, line);
}

static uint64_t now_ms(np_app_t *app)
{
    return app->hal.now_ms(app->hal.ctx);
}

static bool get_str(np_app_t *app, const char *key, char *buf, size_t cap)
{
    buf[0] = 0;
    if (!app->hal.storage_get(app->hal.ctx, key, buf, cap)) {
        buf[0] = 0;
        return false;
    }
    return true;
}

/* Write only when the stored value differs, to spare flash. */
static void put_str(np_app_t *app, const char *key, const char *value)
{
    char old[NP_TOKEN_MAX];
    bool had = app->hal.storage_get(app->hal.ctx, key, old, sizeof(old));
    if (value == NULL) {
        if (had) {
            app->hal.storage_set(app->hal.ctx, key, NULL);
        }
        return;
    }
    if (had && strcmp(old, value) == 0) {
        return;
    }
    app->hal.storage_set(app->hal.ctx, key, value);
}

static void put_u64(np_app_t *app, const char *key, uint64_t value)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
    put_str(app, key, buf);
}

static bool get_u64(np_app_t *app, const char *key, uint64_t *value)
{
    char buf[24];
    if (!get_str(app, key, buf, sizeof(buf)) || !buf[0]) {
        return false;
    }
    *value = strtoull(buf, NULL, 10);
    return true;
}

static uint32_t backoff_ms(int fails)
{
    uint32_t ms = 15000;
    for (int i = 1; i < fails && ms < 600000; i++) {
        ms *= 2;
    }
    return ms > 600000 ? 600000 : ms;
}

static uint32_t clamp_wait(uint64_t ms)
{
    if (ms > MAX_WAIT_MS) {
        return MAX_WAIT_MS;
    }
    return ms < 20 ? 20 : (uint32_t)ms;
}

/* Milliseconds until the panel may be refreshed again; 0 when it may be now. */
static uint32_t cooldown_remaining(np_app_t *app, uint64_t now)
{
    if (!app->has_refreshed) {
        return 0;
    }
    uint64_t min = app->hal.display_min_refresh_interval_ms(app->hal.ctx);
    if (now < app->last_refresh) {
        return 0; /* clock restarted: the old timestamp means nothing */
    }
    uint64_t since = now - app->last_refresh;
    return since >= min ? 0 : (uint32_t)(min - since);
}

static void drain_buttons(np_app_t *app)
{
    while (app->hal.button_poll(app->hal.ctx) != NP_BTN_NONE) {
    }
}

/* ---------- display ---------- */

static int screen_sink(void *user, int half, const uint8_t *data, size_t len)
{
    np_app_t *app = user;
    return app->hal.display_write(app->hal.ctx, half, data, len);
}

/* The frame data is in the panel. Start the refresh and remember what it will show. */
static bool start_refresh(np_app_t *app, const char *screen)
{
    if (app->hal.display_refresh(app->hal.ctx) != 0) {
        logf_(app, NP_LOG_ERROR, "display: refresh refused");
        app->hal.display_power_off(app->hal.ctx);
        return false;
    }
    app->refreshing = true;
    app->refresh_started = now_ms(app);
    snprintf(app->pending_screen, sizeof(app->pending_screen), "%s", screen);
    logf_(app, NP_LOG_INFO, "display: refresh started (%s)", screen);
    return true;
}

static void finish_refresh(np_app_t *app, uint64_t now)
{
    app->hal.display_power_off(app->hal.ctx);
    app->refreshing = false;
    app->has_refreshed = true;
    app->last_refresh = now;
    put_u64(app, "rt_refresh", now);
    snprintf(app->screen, sizeof(app->screen), "%s", app->pending_screen);
    put_str(app, "screen", app->screen);
    if (strcmp(app->screen, NP_SCREEN_FRAME) == 0) {
        snprintf(app->drawn_version, sizeof(app->drawn_version), "%s", app->pending_version);
        app->drawn_page = app->pending_page;
        put_str(app, "dver", app->drawn_version);
        put_u64(app, "dpage", (uint64_t)app->drawn_page);
        if (app->page_interval_min > 0) {
            app->next_page = now + app->page_interval_min * MINUTE_MS;
            put_u64(app, "rt_page", app->next_page);
        }
    }
    logf_(app, NP_LOG_INFO, "display: refresh done after %u ms, showing %s",
          (unsigned)(now - app->refresh_started), app->screen);
}

static void footer_text(np_app_t *app, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s | firmware %s", app->cfg.hardware_id, app->cfg.firmware);
}

typedef int (*render_fn)(np_app_t *app);

/* Render a built-in screen and start the refresh. The caller has checked the cooldown. */
static bool draw_screen(np_app_t *app, const char *name, render_fn render)
{
    if (app->hal.display_begin_frame(app->hal.ctx) != 0) {
        logf_(app, NP_LOG_ERROR, "display: begin_frame failed");
        return false;
    }
    if (render(app) != 0) {
        logf_(app, NP_LOG_ERROR, "display: rendering %s failed", name);
        app->hal.display_power_off(app->hal.ctx);
        return false;
    }
    return start_refresh(app, name);
}

static int render_pairing(np_app_t *app)
{
    char footer[72];
    footer_text(app, footer, sizeof(footer));
    np_pair_view_t view = {
        .qr_text = app->verify_uri_complete,
        .display_url = app->verify_uri,
        .user_code = app->user_code,
        .footer = footer,
    };
    return np_screen_pairing(&view, screen_sink, app);
}

static int render_provisioning(np_app_t *app)
{
    char footer[72];
    footer_text(app, footer, sizeof(footer));
    np_prov_view_t view = {
        .ssid = app->prov.ssid,
        .password = app->prov.password,
        .url = app->prov.url,
        .footer = footer,
    };
    return np_screen_provisioning(&view, screen_sink, app);
}

static const char *message_id; /* argument for render_message */

static int render_message(np_app_t *app)
{
    char footer[72];
    footer_text(app, footer, sizeof(footer));
    np_message_view_t view = { .footer = footer };
    if (strcmp(message_id, NP_SCREEN_MSG_WIFI) == 0) {
        view.title = "No Wi-Fi connection";
        view.lines[0] = "The display cannot join your Wi-Fi network.";
        view.lines[1] = "It keeps trying by itself.";
        view.lines[2] = "To set up Wi-Fi again, hold the Home button";
        view.lines[3] = "while you plug the display in.";
    } else if (strcmp(message_id, NP_SCREEN_MSG_SERVER) == 0) {
        view.title = "Cannot reach the newspaper";
        view.lines[0] = "Wi-Fi works, but the service does not answer.";
        view.lines[1] = "The display keeps trying by itself.";
    } else if (strcmp(message_id, NP_SCREEN_MSG_NO_ISSUE) == 0) {
        view.title = "No issue yet";
        view.lines[0] = "This display is paired.";
        view.lines[1] = "Your first newspaper appears here";
        view.lines[2] = "as soon as it is published.";
    } else {
        view.title = "Pairing paused";
        view.lines[0] = "Nobody added this display in time.";
        view.lines[1] = "Press any button to show a new code.";
    }
    return np_screen_message(&view, screen_sink, app);
}

/* Best effort: skipped when the same message is up, a refresh runs, or the panel must rest. */
static void show_message(np_app_t *app, const char *id)
{
    if (app->refreshing || strcmp(app->screen, id) == 0 || cooldown_remaining(app, now_ms(app)) > 0) {
        return;
    }
    message_id = id;
    draw_screen(app, id, render_message);
}

/* ---------- http ---------- */

static bool body_cb(void *user, const uint8_t *data, size_t len)
{
    np_app_t *app = user;
    if (app->body_len + len >= sizeof(app->body)) {
        app->body_overflow = true;
        return true; /* keep reading so the connection ends cleanly */
    }
    memcpy(app->body + app->body_len, data, len);
    app->body_len += len;
    app->body[app->body_len] = 0;
    return true;
}

#define API_ERR_TRANSPORT (-1)
#define API_ERR_WIFI (-2)

/* cb NULL collects the body as text into app->body. */
static int api_call(np_app_t *app, const char *method, const char *path, bool auth, const char *body,
                    np_http_body_cb cb, np_http_response_t *resp)
{
    resp->status = 0;
    resp->retry_after_s = -1;
    if (!app->online) {
        if (!app->hal.wifi_connect(app->hal.ctx)) {
            logf_(app, NP_LOG_WARN, "wifi: cannot connect");
            return API_ERR_WIFI;
        }
        app->online = true;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s%s", app->api_base, path);

    np_http_header_t headers[6];
    char bearer[NP_TOKEN_MAX + 8];
    char battery[8];
    char rssi[8];
    int n = 0;
    headers[n++] = (np_http_header_t){ "Accept", "application/json, application/octet-stream" };
    if (body) {
        headers[n++] = (np_http_header_t){ "Content-Type", "application/json" };
    }
    if (auth) {
        snprintf(bearer, sizeof(bearer), "Bearer %s", app->token);
        headers[n++] = (np_http_header_t){ "Authorization", bearer };
        headers[n++] = (np_http_header_t){ "X-Device-Firmware", app->cfg.firmware };
        int pct = app->hal.battery_percent ? app->hal.battery_percent(app->hal.ctx) : -1;
        if (pct >= 0) {
            snprintf(battery, sizeof(battery), "%d", pct > 100 ? 100 : pct);
            headers[n++] = (np_http_header_t){ "X-Device-Battery", battery };
        }
        int dbm = app->hal.wifi_rssi ? app->hal.wifi_rssi(app->hal.ctx) : 0;
        if (dbm != 0) {
            snprintf(rssi, sizeof(rssi), "%d", dbm);
            headers[n++] = (np_http_header_t){ "X-Device-Rssi", rssi };
        }
    }

    np_http_request_t req = { .method = method, .url = url, .headers = headers, .header_count = n, .body = body };
    app->body_len = 0;
    app->body[0] = 0;
    app->body_overflow = false;
    int rc = app->hal.http_request(app->hal.ctx, &req, cb ? cb : body_cb, app, resp);
    if (rc != 0) {
        logf_(app, NP_LOG_WARN, "http: %s %s failed (%d)", method, path, rc);
        app->online = false; /* reconnect before the next attempt */
        return API_ERR_TRANSPORT;
    }
    logf_(app, NP_LOG_DEBUG, "http: %s %s -> %d", method, path, resp->status);
    return 0;
}

static void note_failure(np_app_t *app, int rc, bool allow_message)
{
    app->fail_count++;
    if (allow_message && app->fail_count >= 3) {
        show_message(app, rc == API_ERR_WIFI ? NP_SCREEN_MSG_WIFI : NP_SCREEN_MSG_SERVER);
    }
}

/* ---------- pairing ---------- */

static void enter_pairing(np_app_t *app)
{
    app->state = NP_STATE_PAIR_START;
    app->device_code[0] = 0;
    app->user_code[0] = 0;
    app->retry_at = 0;
}

/* The platform says this display is not known any more (401). */
static void forget_token(np_app_t *app)
{
    logf_(app, NP_LOG_WARN, "auth: token rejected, forgetting it and pairing again");
    app->token[0] = 0;
    app->has_issue = false;
    app->version[0] = 0;
    app->drawn_version[0] = 0;
    app->issue_date[0] = 0;
    app->page_count = 0;
    app->drawn_page = 0;
    app->target_page = 1;
    app->next_page = 0;
    put_str(app, "token", NULL);
    put_str(app, "ver", NULL);
    put_str(app, "dver", NULL);
    put_str(app, "dpage", NULL);
    put_str(app, "date", NULL);
    put_str(app, "pages", NULL);
    put_str(app, "rt_check", NULL);
    put_str(app, "rt_page", NULL);
    put_str(app, "rt_tpage", NULL);
    app->pair_attempts = 0;
    enter_pairing(app);
}

static uint32_t pair_start_step(np_app_t *app, uint64_t now)
{
    app->may_sleep = false;
    drain_buttons(app);
    if (app->pair_attempts >= NP_PAIR_MAX_ATTEMPTS) {
        app->state = NP_STATE_PAIR_PAUSED;
        return 0;
    }
    if (now < app->retry_at) {
        return clamp_wait(app->retry_at - now);
    }
    /* Wait for the panel first, so the code's lifetime is not spent on a blank wait. */
    uint32_t rest = cooldown_remaining(app, now);
    if (rest > 0) {
        return clamp_wait(rest);
    }

    char hw[80];
    char fw[48];
    char body[320];
    np_json_quote(app->cfg.hardware_id, hw, sizeof(hw));
    np_json_quote(app->cfg.firmware, fw, sizeof(fw));
    snprintf(body, sizeof(body), "{\"model\":\"%s\",\"hardware_id\":%s,\"firmware\":%s%s}", NP_MODEL_ID, hw, fw,
             app->cfg.emulator ? ",\"emulator\":true" : "");

    np_http_response_t resp;
    int rc = api_call(app, "POST", "/api/v1/devices/pair/start", false, body, NULL, &resp);
    long expires = 900;
    long interval = 5;
    bool ok = rc == 0 && resp.status == 200 && !app->body_overflow &&
              np_json_get_string(app->body, "device_code", app->device_code, sizeof(app->device_code)) &&
              np_json_get_string(app->body, "user_code", app->user_code, sizeof(app->user_code)) &&
              np_json_get_string(app->body, "verification_uri_complete", app->verify_uri_complete,
                                 sizeof(app->verify_uri_complete));
    if (!ok) {
        if (rc == 0) {
            logf_(app, NP_LOG_WARN, "pair: start answered %d", resp.status);
        }
        note_failure(app, rc, true);
        app->retry_at = now_ms(app) + backoff_ms(app->fail_count);
        return 0;
    }
    app->fail_count = 0;
    if (!np_json_get_string(app->body, "verification_uri", app->verify_uri, sizeof(app->verify_uri))) {
        app->verify_uri[0] = 0;
    }
    np_json_get_long(app->body, "expires_in", &expires);
    np_json_get_long(app->body, "interval", &interval);
    if (expires < 30) {
        expires = 30;
    }
    if (interval < 1) {
        interval = 1;
    }
    now = now_ms(app);
    app->pair_deadline = now + (uint64_t)expires * 1000u;
    app->poll_interval_ms = (uint32_t)interval * 1000u;
    app->next_poll = now + app->poll_interval_ms;
    app->state = NP_STATE_PAIR_POLL;
    logf_(app, NP_LOG_INFO, "pair: code %s, open %s", app->user_code, app->verify_uri_complete);
    draw_screen(app, NP_SCREEN_PAIR, render_pairing);
    return 0;
}

static uint32_t pair_poll_step(np_app_t *app, uint64_t now)
{
    app->may_sleep = false;
    drain_buttons(app);
    if (now >= app->pair_deadline) {
        logf_(app, NP_LOG_INFO, "pair: code expired unused");
        app->pair_attempts++;
        enter_pairing(app);
        return 0;
    }
    if (now < app->next_poll) {
        return clamp_wait(app->next_poll - now);
    }

    char code[300];
    char body[320];
    np_json_quote(app->device_code, code, sizeof(code));
    snprintf(body, sizeof(body), "{\"device_code\":%s}", code);
    np_http_response_t resp;
    int rc = api_call(app, "POST", "/api/v1/devices/pair/poll", false, body, NULL, &resp);
    now = now_ms(app);
    app->next_poll = now + app->poll_interval_ms;
    if (rc != 0) {
        app->next_poll += app->poll_interval_ms; /* go easy while the network is down */
        return 0;
    }
    if (resp.status == 410 || resp.status == 404) {
        logf_(app, NP_LOG_INFO, "pair: code no longer valid (%d)", resp.status);
        app->pair_attempts++;
        enter_pairing(app);
        return 0;
    }
    if (resp.status != 200) {
        return 0;
    }
    char status[16];
    if (!np_json_get_string(app->body, "status", status, sizeof(status)) || strcmp(status, "adopted") != 0) {
        return 0; /* pending */
    }
    if (!np_json_get_string(app->body, "token", app->token, sizeof(app->token)) || !app->token[0]) {
        logf_(app, NP_LOG_ERROR, "pair: adopted but no usable token");
        app->token[0] = 0;
        app->pair_attempts++;
        enter_pairing(app);
        return 0;
    }
    put_str(app, "token", app->token);
    char name[64] = "";
    np_json_get_string(app->body, "name", name, sizeof(name));
    logf_(app, NP_LOG_INFO, "pair: adopted as \"%s\"", name);
    app->pair_attempts = 0;
    app->fail_count = 0;
    app->device_code[0] = 0;
    app->state = NP_STATE_RUN;
    app->next_check = 0; /* check state right away */
    app->target_page = 1;
    return 0;
}

static uint32_t pair_paused_step(np_app_t *app, uint64_t now)
{
    if (app->hal.button_poll(app->hal.ctx) != NP_BTN_NONE) {
        drain_buttons(app);
        app->pair_attempts = 0;
        enter_pairing(app);
        return 0;
    }
    if (strcmp(app->screen, NP_SCREEN_MSG_PAUSED) != 0) {
        uint32_t rest = cooldown_remaining(app, now);
        if (rest > 0) {
            app->may_sleep = false;
            return clamp_wait(rest);
        }
        show_message(app, NP_SCREEN_MSG_PAUSED);
        if (app->refreshing) {
            return 0;
        }
    }
    /* A button press wakes the target; after a deep sleep restart pairing begins again. */
    app->may_sleep = true;
    return MAX_WAIT_MS;
}

/* ---------- provisioning ---------- */

static uint32_t provision_step(np_app_t *app, uint64_t now)
{
    app->may_sleep = false;
    drain_buttons(app);
    if (!app->prov_started) {
        memset(&app->prov, 0, sizeof(app->prov));
        if (!app->hal.prov_start(app->hal.ctx, &app->prov)) {
            logf_(app, NP_LOG_ERROR, "wifi: cannot start setup access point");
            return 10000;
        }
        app->prov_started = true;
        logf_(app, NP_LOG_INFO, "wifi: setup access point \"%s\", page %s", app->prov.ssid, app->prov.url);
    }
    if (app->hal.prov_done(app->hal.ctx)) {
        app->hal.prov_stop(app->hal.ctx);
        app->prov_started = false;
        logf_(app, NP_LOG_INFO, "wifi: credentials saved");
        app->state = NP_STATE_BOOT;
        return 0;
    }
    if (strcmp(app->screen, NP_SCREEN_PROVISION) != 0 && cooldown_remaining(app, now) == 0) {
        if (draw_screen(app, NP_SCREEN_PROVISION, render_provisioning)) {
            return 0;
        }
    }
    return 1000;
}

/* ---------- running ---------- */

static void save_target_page(np_app_t *app)
{
    put_u64(app, "rt_tpage", (uint64_t)app->target_page);
}

static void do_check(np_app_t *app)
{
    np_http_response_t resp;
    int rc = api_call(app, "GET", "/api/v1/device/state", true, NULL, NULL, &resp);
    uint64_t now = now_ms(app);
    if (rc == 0 && resp.status == 401) {
        forget_token(app);
        return;
    }
    const char *issue = NULL;
    bool ok = rc == 0 && resp.status == 200 && !app->body_overflow;
    char version[NP_VERSION_MAX] = "";
    char date[16] = "";
    int pages = 0;
    if (ok) {
        issue = np_json_find(app->body, "issue");
        if (issue && !np_json_is_null(issue)) {
            pages = np_json_array_length(np_json_find(issue, "pages"));
            ok = np_json_string(np_json_find(issue, "version"), version, sizeof(version)) && pages >= 1;
            np_json_string(np_json_find(issue, "date"), date, sizeof(date));
        } else if (!np_json_find(app->body, "config")) {
            ok = false; /* not a state document at all */
        }
    }
    if (!ok) {
        if (rc == 0) {
            logf_(app, NP_LOG_WARN, "state: unusable answer (status %d)", resp.status);
        }
        note_failure(app, rc, strcmp(app->screen, NP_SCREEN_FRAME) != 0);
        uint64_t wait = backoff_ms(app->fail_count);
        uint64_t normal = app->check_interval_min * MINUTE_MS;
        app->next_check = now + (wait < normal ? wait : normal);
        put_u64(app, "rt_check", app->next_check);
        return;
    }
    app->fail_count = 0;

    long v;
    if (np_json_get_long(app->body, "config.check_interval_minutes", &v) && v >= 1 && v <= 1440) {
        app->check_interval_min = (uint32_t)v;
    }
    if (np_json_get_long(app->body, "config.page_interval_minutes", &v) && v >= 0 && v <= 1440) {
        if ((uint32_t)v != app->page_interval_min) {
            app->page_interval_min = (uint32_t)v;
            app->next_page = v > 0 ? now + (uint64_t)v * MINUTE_MS : 0;
            put_u64(app, "rt_page", app->next_page);
        }
    }
    put_u64(app, "chk", app->check_interval_min);
    put_u64(app, "pgi", app->page_interval_min);

    if (pages == 0) {
        if (app->has_issue || strcmp(app->screen, NP_SCREEN_MSG_NO_ISSUE) != 0) {
            logf_(app, NP_LOG_INFO, "state: no published issue");
        }
        app->has_issue = false;
        app->page_count = 0;
        app->version[0] = 0;
        put_str(app, "ver", NULL);
        put_str(app, "pages", "0");
        show_message(app, NP_SCREEN_MSG_NO_ISSUE);
    } else {
        if (strcmp(date, app->issue_date) != 0) {
            /* A new day's paper starts on its front page. */
            snprintf(app->issue_date, sizeof(app->issue_date), "%s", date);
            put_str(app, "date", app->issue_date);
            app->target_page = 1;
        }
        if (strcmp(version, app->version) != 0) {
            logf_(app, NP_LOG_INFO, "state: issue %s version %s, %d pages", date, version, pages);
            app->frame_fails = 0;
            app->retry_at = 0;
        }
        snprintf(app->version, sizeof(app->version), "%s", version);
        app->has_issue = true;
        app->page_count = pages;
        if (app->target_page < 1 || app->target_page > pages) {
            app->target_page = 1;
        }
        put_str(app, "ver", app->version);
        put_u64(app, "pages", (uint64_t)pages);
        save_target_page(app);
    }
    app->next_check = now + app->check_interval_min * MINUTE_MS;
    put_u64(app, "rt_check", app->next_check);
}

/*
 * Body callback for the frame download: passes the bytes on to the panel,
 * the first 480,000 to half 0 and the rest to half 1. The HAL sets the
 * response status before the first call, so error bodies are not drawn.
 */
static bool frame_cb(void *user, const uint8_t *data, size_t len)
{
    np_app_t *app = user;
    if (app->frame_resp->status != 200) {
        return true; /* read and drop an error body */
    }
    if (!app->frame_begun) {
        if (app->hal.display_begin_frame(app->hal.ctx) != 0) {
            app->frame_error = true;
            return false;
        }
        app->frame_begun = true;
    }
    while (len > 0) {
        if (app->frame_received >= NP_FRAME_BYTES) {
            app->frame_error = true; /* longer than a frame */
            return false;
        }
        int half = app->frame_received < NP_FRAME_HALF_BYTES ? 0 : 1;
        size_t room = (half == 0 ? NP_FRAME_HALF_BYTES : NP_FRAME_BYTES) - app->frame_received;
        size_t n = len < room ? len : room;
        if (app->hal.display_write(app->hal.ctx, half, data, n) != 0) {
            app->frame_error = true;
            return false;
        }
        app->frame_received += n;
        data += n;
        len -= n;
    }
    return true;
}

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in && n + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
}

/* Download the target page straight into the panel and start the refresh. */
static void fetch_frame(np_app_t *app)
{
    char encoded[NP_VERSION_MAX * 3 + 4];
    char path[sizeof(encoded) + 64];
    url_encode(app->version, encoded, sizeof(encoded));
    snprintf(path, sizeof(path), "/api/v1/device/frame?page=%d&version=%s", app->target_page, encoded);

    np_http_response_t resp;
    app->frame_received = 0;
    app->frame_begun = false;
    app->frame_error = false;
    app->frame_resp = &resp;
    int page = app->target_page;
    logf_(app, NP_LOG_INFO, "frame: fetching page %d", page);
    int rc = api_call(app, "GET", path, true, NULL, frame_cb, &resp);
    app->frame_resp = NULL;
    uint64_t now = now_ms(app);

    if (rc == 0 && resp.status == 200 && !app->frame_error && app->frame_received == NP_FRAME_BYTES) {
        snprintf(app->pending_version, sizeof(app->pending_version), "%s", app->version);
        app->pending_page = page;
        if (start_refresh(app, NP_SCREEN_FRAME)) {
            app->frame_fails = 0;
            app->fail_count = 0;
            return;
        }
        app->frame_fails++;
        app->retry_at = now + backoff_ms(app->frame_fails);
        return;
    }

    if (app->frame_begun) {
        app->hal.display_power_off(app->hal.ctx); /* drop the partial frame, the old picture stays */
    }
    if (rc == 0 && resp.status == 401) {
        forget_token(app);
        return;
    }
    app->frame_fails++;
    if (rc == 0 && resp.status == 503) {
        int s = resp.retry_after_s > 0 ? resp.retry_after_s : 10;
        if (s > 600) {
            s = 600;
        }
        logf_(app, NP_LOG_INFO, "frame: not rendered yet, retry in %d s", s);
        app->retry_at = now + (uint64_t)s * 1000u;
        return;
    }
    if (rc == 0 && resp.status == 404) {
        /* The issue changed under us. Ask for the state again and start from page 1. */
        logf_(app, NP_LOG_WARN, "frame: page %d does not exist", page);
        app->target_page = 1;
        save_target_page(app);
        app->next_check = now;
    } else if (rc == 0 && resp.status == 200) {
        logf_(app, NP_LOG_WARN, "frame: bad length %u, expected %u", (unsigned)app->frame_received,
              (unsigned)NP_FRAME_BYTES);
    } else if (rc == 0) {
        logf_(app, NP_LOG_WARN, "frame: status %d", resp.status);
    }
    app->retry_at = now + backoff_ms(app->frame_fails);
}

static bool want_draw(const np_app_t *app)
{
    if (!app->has_issue || app->page_count < 1) {
        return false;
    }
    return strcmp(app->screen, NP_SCREEN_FRAME) != 0 || strcmp(app->drawn_version, app->version) != 0 ||
           app->drawn_page != app->target_page;
}

static uint32_t run_step(np_app_t *app, uint64_t now)
{
    bool force_check = false;
    bool pressed = false;
    np_button_t b;
    while ((b = app->hal.button_poll(app->hal.ctx)) != NP_BTN_NONE) {
        if (!app->has_issue || app->page_count < 1) {
            force_check = true; /* nothing to turn: use the press to look for an issue */
            continue;
        }
        if (b == NP_BTN_NEXT) {
            app->target_page = app->target_page % app->page_count + 1;
        } else if (b == NP_BTN_BACK) {
            app->target_page = app->target_page <= 1 ? app->page_count : app->target_page - 1;
        } else {
            app->target_page = 1;
        }
        pressed = true;
    }
    if (pressed) {
        logf_(app, NP_LOG_INFO, "button: go to page %d", app->target_page);
        save_target_page(app);
        app->retry_at = 0;
        if (app->page_interval_min > 0) {
            app->next_page = now + app->page_interval_min * MINUTE_MS;
            put_u64(app, "rt_page", app->next_page);
        }
    }

    if (force_check || now >= app->next_check) {
        do_check(app);
        if (app->state != NP_STATE_RUN || app->refreshing) {
            return 0;
        }
        now = now_ms(app);
    }

    /* Page timer. It waits while a page turn is still pending. */
    if (app->has_issue && app->page_interval_min > 0 && app->page_count > 1) {
        if (app->next_page == 0) {
            app->next_page = now + app->page_interval_min * MINUTE_MS;
            put_u64(app, "rt_page", app->next_page);
        } else if (now >= app->next_page && !want_draw(app)) {
            app->target_page = app->target_page % app->page_count + 1;
            save_target_page(app);
            app->next_page = now + app->page_interval_min * MINUTE_MS;
            put_u64(app, "rt_page", app->next_page);
            logf_(app, NP_LOG_INFO, "timer: turn to page %d", app->target_page);
        }
    }

    uint64_t wake = app->next_check;
    if (want_draw(app)) {
        uint64_t allowed = now + cooldown_remaining(app, now);
        if (app->retry_at > allowed) {
            allowed = app->retry_at;
        }
        if (allowed <= now) {
            fetch_frame(app);
            return 0;
        }
        if (allowed < wake) {
            wake = allowed;
        }
    } else if (app->has_issue && app->page_interval_min > 0 && app->page_count > 1 && app->next_page < wake) {
        wake = app->next_page;
    }
    app->may_sleep = true;
    return clamp_wait(wake > now ? wake - now : 0);
}

/* ---------- entry points ---------- */

static void boot(np_app_t *app)
{
    uint64_t v;
    get_str(app, "screen", app->screen, sizeof(app->screen));
    if (get_u64(app, "rt_refresh", &v)) {
        app->has_refreshed = true;
        app->last_refresh = v;
    }
    if (!app->hal.wifi_has_credentials(app->hal.ctx)) {
        app->state = NP_STATE_PROVISION;
        return;
    }
    get_str(app, "token", app->token, sizeof(app->token));
    if (!app->token[0]) {
        enter_pairing(app);
        return;
    }
    get_str(app, "ver", app->version, sizeof(app->version));
    get_str(app, "dver", app->drawn_version, sizeof(app->drawn_version));
    get_str(app, "date", app->issue_date, sizeof(app->issue_date));
    app->page_count = get_u64(app, "pages", &v) ? (int)v : 0;
    app->drawn_page = get_u64(app, "dpage", &v) ? (int)v : 0;
    app->check_interval_min = get_u64(app, "chk", &v) && v >= 1 ? (uint32_t)v : DEFAULT_CHECK_MIN;
    app->page_interval_min = get_u64(app, "pgi", &v) ? (uint32_t)v : DEFAULT_PAGE_MIN;
    app->has_issue = app->page_count > 0 && app->version[0];
    app->target_page = get_u64(app, "rt_tpage", &v) ? (int)v : (app->drawn_page > 0 ? app->drawn_page : 1);
    if (app->target_page < 1 || (app->page_count > 0 && app->target_page > app->page_count)) {
        app->target_page = 1;
    }
    app->next_check = get_u64(app, "rt_check", &v) ? v : 0;
    app->next_page = get_u64(app, "rt_page", &v) ? v : 0;
    app->state = NP_STATE_RUN;
}

void np_app_init(np_app_t *app, const np_hal_t *hal, const np_app_config_t *config)
{
    memset(app, 0, sizeof(*app));
    app->hal = *hal;
    app->cfg = *config;
    snprintf(app->api_base, sizeof(app->api_base), "%s", config->api_base ? config->api_base : "");
    size_t n = strlen(app->api_base);
    while (n > 0 && app->api_base[n - 1] == '/') {
        app->api_base[--n] = 0;
    }
    app->state = NP_STATE_BOOT;
    app->check_interval_min = DEFAULT_CHECK_MIN;
    app->page_interval_min = DEFAULT_PAGE_MIN;
    app->target_page = 1;
}

uint32_t np_app_step(np_app_t *app)
{
    uint64_t now = now_ms(app);
    app->may_sleep = false;

    if (app->refreshing) {
        /* Never interrupt a refresh. Button presses during it are dropped. */
        drain_buttons(app);
        bool busy = app->hal.display_busy(app->hal.ctx);
        if (busy && now - app->refresh_started < NP_REFRESH_TIMEOUT_MS) {
            return BUSY_POLL_MS;
        }
        if (busy) {
            logf_(app, NP_LOG_ERROR, "display: still busy after %u ms, giving up waiting",
                  (unsigned)NP_REFRESH_TIMEOUT_MS);
        }
        finish_refresh(app, now);
        return 0;
    }

    switch (app->state) {
    case NP_STATE_BOOT:
        boot(app);
        return 0;
    case NP_STATE_PROVISION:
        return provision_step(app, now);
    case NP_STATE_PAIR_START:
        return pair_start_step(app, now);
    case NP_STATE_PAIR_POLL:
        return pair_poll_step(app, now);
    case NP_STATE_PAIR_PAUSED:
        return pair_paused_step(app, now);
    case NP_STATE_RUN:
        return run_step(app, now);
    }
    return 1000;
}

void np_app_run_once(np_app_t *app)
{
    uint32_t ms = np_app_step(app);
    if (ms > 0) {
        app->hal.idle(app->hal.ctx, ms, app->may_sleep);
    }
}
