/*
 * Host unit tests for the portable core. No test framework: each test is a
 * function, `core_tests <name>` runs one, `core_tests` runs all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "fake_hal.h"
#include "frame.h"
#include "json.h"
#include "screen.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_INT(actual, expected)                                                              \
    do {                                                                                         \
        long long a_ = (long long)(actual), e_ = (long long)(expected);                          \
        if (a_ != e_) {                                                                          \
            printf("  FAIL %s:%d: %s is %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); \
            failures++;                                                                          \
        }                                                                                        \
    } while (0)

static np_app_t app; /* large (it holds the JSON buffer), so not on the stack */
static const np_app_config_t CONFIG = {
    .api_base = "https://newspaper.brushknight.com/",
    .hardware_id = "test-0001",
    .firmware = "0.0.0-test",
    .emulator = false,
};

static void start(fake_t *f)
{
    np_hal_t hal = fake_hal(f);
    np_app_init(&app, &hal, &CONFIG);
}

/* Run the app for `ms` of fake time, the way a target's main loop would. */
static void run_for(fake_t *f, uint64_t ms)
{
    uint64_t end = f->now + ms;
    int spins = 0;
    while (f->now < end) {
        uint32_t wait = np_app_step(&app);
        if (wait == 0) {
            if (++spins > 10000) {
                printf("  FAIL: app is spinning without waiting\n");
                failures++;
                return;
            }
            continue;
        }
        spins = 0;
        uint64_t left = end - f->now;
        f->now += wait < left ? wait : left;
    }
}

/* Pair and draw page 1. */
static void pair_and_draw(fake_t *f)
{
    start(f);
    run_for(f, 20000);
    f->adopted = true;
    run_for(f, 240000);
}

static bool frame_is_legal(const uint8_t *frame)
{
    for (size_t i = 0; i < NP_FRAME_BYTES; i++) {
        uint8_t hi = frame[i] >> 4, lo = frame[i] & 15;
        if (hi == 4 || hi > 6 || lo == 4 || lo > 6) {
            return false;
        }
    }
    return true;
}

static bool shows_page(const fake_t *f, int page)
{
    for (size_t i = 0; i < NP_FRAME_BYTES; i++) {
        if (f->shown[i] != fake_frame_byte(page, i)) {
            return false;
        }
    }
    return true;
}

/* ---------------- tests ---------------- */

static void test_json(void)
{
    const char *doc = "{ \"a\": { \"b\": \"x\\\"y\\u0041\", \"n\": -42, \"arr\": [1, {\"k\": [2,3]}, \"s,]\"] },"
                      " \"z\": null, \"t\": \"end\" }";
    char buf[32];
    long n = 0;
    CHECK(np_json_get_string(doc, "a.b", buf, sizeof(buf)) && strcmp(buf, "x\"yA") == 0);
    CHECK(np_json_get_long(doc, "a.n", &n) && n == -42);
    CHECK_INT(np_json_array_length(np_json_find(doc, "a.arr")), 3);
    CHECK(np_json_is_null(np_json_find(doc, "z")));
    CHECK(np_json_get_string(doc, "t", buf, sizeof(buf)) && strcmp(buf, "end") == 0);
    CHECK(np_json_find(doc, "a.missing") == NULL);
    CHECK(np_json_find(doc, "k") == NULL); /* nested keys are not found at the top level */
    CHECK(!np_json_get_string(doc, "a.b", buf, 3)); /* too small */
    CHECK(np_json_find("{\"a\": \"unterminated", "b") == NULL);
    CHECK(np_json_quote("a\"b\\c\n", buf, sizeof(buf)) && strcmp(buf, "\"a\\\"b\\\\c\\u000a\"") == 0);
}

static void test_pairing_flow(void)
{
    fake_t f;
    fake_init(&f);
    start(&f);
    run_for(&f, 1000);
    CHECK_INT(f.start_calls, 1);
    CHECK(strstr(f.last_body, "\"model\":\"inky-impression-13.3-spectra6\"") != NULL);
    CHECK(strstr(f.last_body, "\"hardware_id\":\"test-0001\"") != NULL);
    CHECK(!f.saw_emulator_flag);
    CHECK_INT(app.state, NP_STATE_PAIR_POLL);
    CHECK(strcmp(app.user_code, "K7QF-3MXB") == 0);
    /* The pairing screen went to the panel as one complete, legal frame. */
    CHECK_INT(f.refresh_count, 1);
    CHECK_INT(f.half_bytes[0], NP_FRAME_HALF_BYTES);
    CHECK_INT(f.half_bytes[1], NP_FRAME_HALF_BYTES);
    CHECK(frame_is_legal(f.shown));

    run_for(&f, 60000);
    CHECK(f.poll_calls >= 8 && f.poll_calls <= 12);
    CHECK(f.min_poll_gap >= 5000); /* never faster than `interval` */
    CHECK(fake_get(&f, "token") == NULL);
    CHECK(strcmp(fake_get(&f, "screen"), NP_SCREEN_PAIR) == 0);

    f.adopted = true;
    run_for(&f, 6000);
    CHECK_INT(app.state, NP_STATE_RUN);
    CHECK(fake_get(&f, "token") && strcmp(fake_get(&f, "token"), "newspaper_dev_testtoken") == 0);
    int polls = f.poll_calls;
    CHECK_INT(f.state_calls, 1);
    CHECK(f.saw_auth_header);

    /* The first frame has to wait for the panel's minimum interval, then it is drawn. */
    run_for(&f, 300000);
    CHECK_INT(f.poll_calls, polls); /* polling stopped */
    CHECK_INT(f.frame_calls, 1);
    CHECK_INT(f.refresh_count, 2);
    CHECK(shows_page(&f, 1));
    CHECK(strcmp(fake_get(&f, "screen"), NP_SCREEN_FRAME) == 0);
    CHECK(strcmp(fake_get(&f, "dver"), "v1") == 0);
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_emulator_flag(void)
{
    fake_t f;
    fake_init(&f);
    np_hal_t hal = fake_hal(&f);
    np_app_config_t cfg = CONFIG;
    cfg.emulator = true;
    np_app_init(&app, &hal, &cfg);
    run_for(&f, 1000);
    CHECK(f.saw_emulator_flag);
    fake_free(&f);
}

static void test_pairing_expiry(void)
{
    fake_t f;
    fake_init(&f);
    f.pair_expires_in = 300;
    start(&f);
    run_for(&f, 290000);
    CHECK_INT(f.start_calls, 1);
    run_for(&f, 30000);
    CHECK_INT(f.start_calls, 2); /* a new code after the first one ran out */
    CHECK(strcmp(app.user_code, "K7QF-3MXC") == 0);
    CHECK_INT(f.refresh_count, 2);

    /* Nobody ever adopts: after NP_PAIR_MAX_ATTEMPTS codes the display stops asking. */
    run_for(&f, 3600000);
    CHECK_INT(f.start_calls, NP_PAIR_MAX_ATTEMPTS);
    CHECK_INT(app.state, NP_STATE_PAIR_PAUSED);
    CHECK(strcmp(fake_get(&f, "screen"), NP_SCREEN_MSG_PAUSED) == 0);
    CHECK(app.may_sleep);
    int polls = f.poll_calls;
    run_for(&f, 3600000);
    CHECK_INT(f.poll_calls, polls);

    /* Any button brings a fresh code. */
    fake_press(&f, NP_BTN_HOME);
    run_for(&f, 5000);
    CHECK_INT(f.start_calls, NP_PAIR_MAX_ATTEMPTS + 1);
    CHECK_INT(app.state, NP_STATE_PAIR_POLL);
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_repair_on_401(void)
{
    fake_t f;
    fake_init(&f);
    pair_and_draw(&f);
    CHECK_INT(app.state, NP_STATE_RUN);
    CHECK(shows_page(&f, 1));

    f.token_valid = false; /* removed in the cloud */
    run_for(&f, 31 * 60000);
    CHECK(fake_get(&f, "token") == NULL);
    CHECK(fake_get(&f, "dver") == NULL);
    CHECK_INT(f.start_calls, 2);
    CHECK_INT(app.state, NP_STATE_PAIR_POLL);
    CHECK(strcmp(fake_get(&f, "screen"), NP_SCREEN_PAIR) == 0);

    /* And a 401 on the frame request does the same. */
    f.adopted = true;
    run_for(&f, 300000);
    CHECK_INT(app.state, NP_STATE_RUN);
    CHECK(shows_page(&f, 1));
    f.token_valid = false;
    fake_press(&f, NP_BTN_NEXT);
    run_for(&f, 10000);
    CHECK(fake_get(&f, "token") == NULL);
    CHECK_INT(f.start_calls, 3);
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_version_change_redraws(void)
{
    fake_t f;
    fake_init(&f);
    pair_and_draw(&f);
    CHECK_INT(f.frame_calls, 1);
    int refreshes = f.refresh_count;

    /* Same version: state is checked, nothing is fetched or drawn. */
    run_for(&f, 95 * 60000);
    CHECK(f.state_calls >= 4);
    CHECK_INT(f.frame_calls, 1);
    CHECK_INT(f.refresh_count, refreshes);

    snprintf(f.issue_version, sizeof(f.issue_version), "v2 with spaces&stuff");
    run_for(&f, 31 * 60000);
    CHECK_INT(f.frame_calls, 2);
    CHECK_INT(f.refresh_count, refreshes + 1);
    CHECK(strcmp(fake_get(&f, "dver"), "v2 with spaces&stuff") == 0);

    /* A new date goes back to the front page. */
    fake_press(&f, NP_BTN_NEXT);
    run_for(&f, 240000);
    CHECK(shows_page(&f, 2));
    snprintf(f.issue_version, sizeof(f.issue_version), "v3");
    snprintf(f.issue_date, sizeof(f.issue_date), "2026-11-12");
    run_for(&f, 31 * 60000);
    CHECK(shows_page(&f, 1));

    /* The issue disappears: the "no issue" screen is drawn once. */
    f.issue_present = false;
    refreshes = f.refresh_count;
    run_for(&f, 65 * 60000);
    CHECK_INT(f.refresh_count, refreshes + 1);
    CHECK(strcmp(fake_get(&f, "screen"), NP_SCREEN_MSG_NO_ISSUE) == 0);
    CHECK(frame_is_legal(f.shown));

    /* And comes back. */
    f.issue_present = true;
    run_for(&f, 31 * 60000);
    CHECK(shows_page(&f, 1));
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_buttons_wrap(void)
{
    fake_t f;
    fake_init(&f);
    pair_and_draw(&f);
    CHECK_INT(f.frame_calls, 1);

    static const struct {
        np_button_t button;
        int page;
    } steps[] = {
        { NP_BTN_NEXT, 2 }, { NP_BTN_NEXT, 3 }, { NP_BTN_NEXT, 1 }, /* wraps forward */
        { NP_BTN_BACK, 3 },                                        /* wraps backward */
        { NP_BTN_BACK, 2 }, { NP_BTN_HOME, 1 },
    };
    int calls = f.frame_calls;
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        fake_press(&f, steps[i].button);
        run_for(&f, 200000);
        calls++;
        CHECK_INT(f.frame_calls, calls);
        CHECK_INT(f.frame_pages[calls - 1], steps[i].page);
        CHECK(shows_page(&f, steps[i].page));
        CHECK_INT(atoi(fake_get(&f, "dpage")), steps[i].page);
    }

    /* Home on page 1 changes nothing. */
    fake_press(&f, NP_BTN_HOME);
    run_for(&f, 200000);
    CHECK_INT(f.frame_calls, calls);

    /* A press during a refresh is ignored. */
    fake_press(&f, NP_BTN_NEXT);
    run_for(&f, 2000);
    CHECK_INT(f.frame_calls, calls + 1);
    CHECK(app.refreshing);
    fake_press(&f, NP_BTN_NEXT);
    fake_press(&f, NP_BTN_NEXT);
    run_for(&f, 400000);
    CHECK_INT(f.frame_calls, calls + 1);
    CHECK(shows_page(&f, 2));
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_min_refresh_interval(void)
{
    fake_t f;
    fake_init(&f);
    f.page_interval_min = 1; /* asks for a page turn every minute: faster than the panel allows */
    pair_and_draw(&f);
    run_for(&f, 20 * 60000);
    /* Impatient presses in the cooldown collapse into one draw. */
    for (int i = 0; i < 5; i++) {
        fake_press(&f, NP_BTN_NEXT);
        run_for(&f, 3000);
    }
    run_for(&f, 20 * 60000);

    CHECK(f.refresh_count >= 8);
    CHECK_INT(f.violations, 0);
    int n = f.refresh_count < FAKE_MAX_LOG ? f.refresh_count : FAKE_MAX_LOG;
    for (int i = 1; i < n; i++) {
        uint64_t gap = f.refresh_started[i] - f.refresh_started[i - 1];
        /* previous refresh (15 s) plus the 180 s rest */
        CHECK(gap >= (uint64_t)f.refresh_ms + f.min_interval_ms);
    }
    fake_free(&f);
}

static void test_page_timer(void)
{
    fake_t f;
    fake_init(&f);
    f.page_interval_min = 10;
    pair_and_draw(&f);
    CHECK(shows_page(&f, 1));
    run_for(&f, 10 * 60000 + 30000);
    CHECK(shows_page(&f, 2));
    run_for(&f, 10 * 60000);
    CHECK(shows_page(&f, 3));
    run_for(&f, 10 * 60000);
    CHECK(shows_page(&f, 1)); /* wraps */
    CHECK(app.may_sleep);

    /* 0 means stay on the page. */
    f.page_interval_min = 0;
    run_for(&f, 31 * 60000);
    int calls = f.frame_calls;
    run_for(&f, 120 * 60000);
    CHECK_INT(f.frame_calls, calls);
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_frame_chunks_and_halves(void)
{
    static const size_t patterns[][4] = {
        { 4096, 0, 0, 0 }, { 1, 7777, 13, 20000 }, { 480000 / 3 + 1, 0, 0, 0 }, { 999, 1001, 0, 0 },
    };
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        fake_t f;
        fake_init(&f);
        memcpy(f.chunk_sizes, patterns[p], sizeof(f.chunk_sizes));
        pair_and_draw(&f);
        CHECK_INT(f.frame_calls, 1);
        CHECK_INT(f.half_bytes[0], NP_FRAME_HALF_BYTES);
        CHECK_INT(f.half_bytes[1], NP_FRAME_HALF_BYTES);
        CHECK(shows_page(&f, 1)); /* every byte in the right half at the right offset */
        CHECK(f.max_write <= 20000); /* streamed, never the whole frame at once */
        CHECK_INT(f.violations, 0);
        fake_free(&f);
    }
}

static void test_frame_errors(void)
{
    fake_t f;
    fake_init(&f);
    f.frame_503_left = 2;
    pair_and_draw(&f);
    /* Two 503 answers with Retry-After 7, then the frame. */
    CHECK_INT(f.frame_calls, 3);
    CHECK(shows_page(&f, 1));

    /* A short frame is never refreshed; the old picture stays. */
    int refreshes = f.refresh_count;
    f.frame_truncate = 500000;
    fake_press(&f, NP_BTN_NEXT);
    run_for(&f, 400000);
    CHECK_INT(f.refresh_count, refreshes);
    CHECK(shows_page(&f, 1));
    CHECK(f.frame_calls > 3);
    f.frame_truncate = 0;
    run_for(&f, 700000);
    CHECK(shows_page(&f, 2));

    /* Network down: nothing is drawn over the newspaper, and it recovers. */
    refreshes = f.refresh_count;
    f.net_down = true;
    fake_press(&f, NP_BTN_NEXT);
    run_for(&f, 60 * 60000);
    CHECK_INT(f.refresh_count, refreshes);
    CHECK(strcmp(fake_get(&f, "screen"), NP_SCREEN_FRAME) == 0);
    f.net_down = false;
    run_for(&f, 31 * 60000);
    CHECK(shows_page(&f, 3));
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_restart_restores(void)
{
    fake_t f;
    fake_init(&f);
    f.page_interval_min = 10;
    pair_and_draw(&f);
    fake_press(&f, NP_BTN_NEXT);
    run_for(&f, 240000);
    CHECK(shows_page(&f, 2));
    int frames = f.frame_calls, refreshes = f.refresh_count, starts = f.start_calls, states = f.state_calls;

    /* A deep sleep wake is a restart: a new app on the same storage and clock. */
    start(&f);
    run_for(&f, 60000);
    CHECK_INT(app.state, NP_STATE_RUN);
    CHECK_INT(f.start_calls, starts);
    CHECK_INT(f.frame_calls, frames); /* nothing redrawn */
    CHECK_INT(f.refresh_count, refreshes);
    CHECK_INT(f.state_calls, states); /* the state check is not due yet */

    /* Woken by a button: that page is drawn. */
    start(&f);
    fake_press(&f, NP_BTN_NEXT);
    run_for(&f, 240000);
    CHECK(shows_page(&f, 3));

    /* Woken by the page timer. */
    start(&f);
    run_for(&f, 11 * 60000);
    CHECK(shows_page(&f, 1));
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_provisioning(void)
{
    fake_t f;
    fake_init(&f);
    f.wifi_creds = false;
    start(&f);
    run_for(&f, 30000);
    CHECK_INT(app.state, NP_STATE_PROVISION);
    CHECK_INT(f.prov_start_calls, 1);
    CHECK_INT(f.start_calls, 0); /* no network use before Wi-Fi is set up */
    CHECK_INT(f.refresh_count, 1);
    CHECK(frame_is_legal(f.shown));
    CHECK(strcmp(fake_get(&f, "screen"), NP_SCREEN_PROVISION) == 0);
    run_for(&f, 600000);
    CHECK_INT(f.refresh_count, 1); /* drawn once, not again and again */

    f.prov_finished = true;
    run_for(&f, 5000);
    CHECK_INT(app.state, NP_STATE_PAIR_POLL);
    CHECK_INT(f.start_calls, 1);
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

static void test_wifi_failure(void)
{
    fake_t f;
    fake_init(&f);
    f.wifi_ok = false;
    start(&f);
    run_for(&f, 10 * 60000);
    CHECK_INT(f.start_calls, 0);
    CHECK(strcmp(fake_get(&f, "screen"), NP_SCREEN_MSG_WIFI) == 0);
    CHECK_INT(f.refresh_count, 1);
    f.wifi_ok = true;
    run_for(&f, 11 * 60000);
    CHECK_INT(f.start_calls, 1);
    CHECK_INT(app.state, NP_STATE_PAIR_POLL);
    CHECK_INT(f.violations, 0);
    fake_free(&f);
}

/* ---- setup screens ---- */

typedef struct {
    uint8_t *frame;
    size_t got[2];
    int order_errors;
} collect_t;

static int collect_sink(void *user, int half, const uint8_t *data, size_t len)
{
    collect_t *c = user;
    if (half == 1 && c->got[0] != NP_FRAME_HALF_BYTES) {
        c->order_errors++;
    }
    if (c->got[half] + len > NP_FRAME_HALF_BYTES) {
        c->order_errors++;
        return -1;
    }
    memcpy(c->frame + (size_t)half * NP_FRAME_HALF_BYTES + c->got[half], data, len);
    c->got[half] += len;
    return 0;
}

static int pixel(const uint8_t *frame, int x, int y)
{
    int half = x >= NP_HALF_WIDTH;
    int hx = x - half * NP_HALF_WIDTH;
    uint8_t b = frame[(size_t)half * NP_FRAME_HALF_BYTES + (size_t)y * NP_ROW_BYTES + (size_t)(hx / 2)];
    return (hx & 1) ? (b & 15) : (b >> 4);
}

static void check_screen(collect_t *c, const char *name, bool expect_accent)
{
    CHECK_INT(c->got[0], NP_FRAME_HALF_BYTES);
    CHECK_INT(c->got[1], NP_FRAME_HALF_BYTES);
    CHECK_INT(c->order_errors, 0);
    CHECK(frame_is_legal(c->frame));
    long counts[8] = { 0 };
    for (int y = 0; y < NP_PANEL_HEIGHT; y++) {
        for (int x = 0; x < NP_PANEL_WIDTH; x++) {
            counts[pixel(c->frame, x, y)]++;
        }
    }
    long total = (long)NP_PANEL_WIDTH * NP_PANEL_HEIGHT;
    CHECK(counts[NP_INK_WHITE] > total / 2);   /* mostly paper */
    CHECK(counts[NP_INK_BLACK] > total / 100); /* with real content */
    CHECK_INT(counts[NP_INK_BLUE] + counts[NP_INK_GREEN], 0);
    if (expect_accent) {
        CHECK(counts[NP_INK_YELLOW] > 1000 && counts[NP_INK_RED] > 1000);
    }
    CHECK_INT(pixel(c->frame, 0, 0), NP_INK_WHITE);

    /* NP_TEST_DUMP_DIR=<dir> writes the raw frames; tools/frame2png.js turns them into pictures. */
    const char *dir = getenv("NP_TEST_DUMP_DIR");
    if (dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.el133uf1", dir, name);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            fwrite(c->frame, 1, NP_FRAME_BYTES, fp);
            fclose(fp);
        }
    }
    c->got[0] = c->got[1] = 0;
}

static void test_setup_screens(void)
{
    collect_t c = { .frame = calloc(1, NP_FRAME_BYTES) };

    np_pair_view_t pair = {
        .qr_text = "https://newspaper.brushknight.com/app/pair?code=K7QF-3MXD",
        .display_url = "https://newspaper.brushknight.com/app/pair",
        .user_code = "K7QF-3MXD",
        .footer = "esp32-a1b2c3d4e5f6 | firmware 0.1.0",
    };
    CHECK_INT(np_screen_pairing(&pair, collect_sink, &c), 0);
    /* The QR code sits in the middle of the top half: look for its finder pattern rhythm. */
    int transitions = 0;
    for (int x = 300; x < 900; x++) {
        if ((pixel(c.frame, x, 500) == NP_INK_BLACK) != (pixel(c.frame, x + 1, 500) == NP_INK_BLACK)) {
            transitions++;
        }
    }
    CHECK(transitions >= 4);
    check_screen(&c, "pairing", true);

    np_prov_view_t prov = { .ssid = "Newspaper-A1B2", .password = "", .url = "http://192.168.4.1",
                            .footer = "esp32-a1b2c3d4e5f6 | firmware 0.1.0" };
    CHECK_INT(np_screen_provisioning(&prov, collect_sink, &c), 0);
    check_screen(&c, "provisioning", true);

    np_message_view_t msg = { .title = "No issue yet",
                              .lines = { "This display is paired.", "Your first newspaper appears here" },
                              .footer = "x" };
    CHECK_INT(np_screen_message(&msg, collect_sink, &c), 0);
    check_screen(&c, "message", true);

    /* A URL too long for the QR size limit is refused instead of drawn wrong. */
    char longurl[400];
    memset(longurl, 'a', sizeof(longurl) - 1);
    longurl[sizeof(longurl) - 1] = 0;
    pair.qr_text = longurl;
    CHECK(np_screen_pairing(&pair, collect_sink, &c) != 0);
    free(c.frame);
}

/* ---------------- runner ---------------- */

static const struct {
    const char *name;
    void (*fn)(void);
} TESTS[] = {
    { "json", test_json },
    { "pairing_flow", test_pairing_flow },
    { "emulator_flag", test_emulator_flag },
    { "pairing_expiry", test_pairing_expiry },
    { "repair_on_401", test_repair_on_401 },
    { "version_change_redraws", test_version_change_redraws },
    { "buttons_wrap", test_buttons_wrap },
    { "min_refresh_interval", test_min_refresh_interval },
    { "page_timer", test_page_timer },
    { "frame_chunks_and_halves", test_frame_chunks_and_halves },
    { "frame_errors", test_frame_errors },
    { "restart_restores", test_restart_restores },
    { "provisioning", test_provisioning },
    { "wifi_failure", test_wifi_failure },
    { "setup_screens", test_setup_screens },
};

int main(int argc, char **argv)
{
    int ran = 0;
    for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        if (argc > 1 && strcmp(argv[1], "--list") == 0) {
            printf("%s\n", TESTS[i].name);
            continue;
        }
        if (argc > 1 && strcmp(argv[1], TESTS[i].name) != 0) {
            continue;
        }
        int before = failures;
        printf("%s\n", TESTS[i].name);
        TESTS[i].fn();
        printf("  %s\n", failures == before ? "ok" : "FAILED");
        ran++;
    }
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        return 0;
    }
    if (ran == 0) {
        printf("no such test\n");
        return 2;
    }
    return failures ? 1 : 0;
}
