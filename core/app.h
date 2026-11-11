/*
 * The Morning Newspaper display application: one state machine shared by the
 * ESP32 firmware, the browser emulator and the host tests.
 * Behaviour follows docs/PROTOCOL.md section 3.
 */
#ifndef NP_APP_H
#define NP_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *api_base;    /* for example https://newspaper.brushknight.com, no trailing slash needed */
    const char *hardware_id; /* stable label, for example esp32-a1b2c3d4e5f6 */
    const char *firmware;    /* version string */
    bool emulator;           /* adds "emulator": true to the pairing request */
} np_app_config_t;

typedef enum {
    NP_STATE_BOOT = 0,
    NP_STATE_PROVISION,   /* no Wi-Fi credentials: setup access point is up */
    NP_STATE_PAIR_START,  /* about to ask the platform for a pairing code */
    NP_STATE_PAIR_POLL,   /* QR and user code on screen, polling */
    NP_STATE_PAIR_PAUSED, /* several codes expired unused: waiting for a button */
    NP_STATE_RUN          /* paired: state checks, frames, page timer, buttons */
} np_state_t;

/* Names stored under the "screen" key: what the panel shows right now. */
#define NP_SCREEN_FRAME "frame"
#define NP_SCREEN_PAIR "pair"
#define NP_SCREEN_PROVISION "prov"
#define NP_SCREEN_MSG_WIFI "m_wifi"
#define NP_SCREEN_MSG_SERVER "m_server"
#define NP_SCREEN_MSG_NO_ISSUE "m_noissue"
#define NP_SCREEN_MSG_PAUSED "m_paused"

/* How many pairing codes may expire unused before the display stops asking. */
#define NP_PAIR_MAX_ATTEMPTS 4
/* A refresh that still reports busy after this long is treated as finished. */
#define NP_REFRESH_TIMEOUT_MS 90000u

#define NP_TOKEN_MAX 200
#define NP_VERSION_MAX 160
#define NP_BODY_MAX 8192

typedef struct np_app {
    np_hal_t hal;
    np_app_config_t cfg;
    char api_base[128];

    np_state_t state;
    bool may_sleep; /* valid after np_app_step: the target may power down while waiting */
    bool online;
    int fail_count;

    /* panel */
    bool refreshing;
    uint64_t refresh_started;
    bool has_refreshed;
    uint64_t last_refresh;
    char screen[16];         /* what the panel shows */
    char pending_screen[16]; /* what it will show when the running refresh ends */

    /* provisioning */
    bool prov_started;
    np_prov_info_t prov;

    /* pairing */
    char device_code[256];
    char user_code[16];
    char verify_uri[128];
    char verify_uri_complete[160];
    uint64_t pair_deadline;
    uint64_t next_poll;
    uint32_t poll_interval_ms;
    int pair_attempts;

    /* running */
    char token[NP_TOKEN_MAX];
    bool has_issue;
    char version[NP_VERSION_MAX];       /* latest issue version known from the platform */
    char drawn_version[NP_VERSION_MAX]; /* version of the frame on the panel */
    char pending_version[NP_VERSION_MAX];
    char issue_date[16];
    int page_count;
    int drawn_page;
    int target_page; /* page that should be on the panel */
    int pending_page;
    uint32_t check_interval_min;
    uint32_t page_interval_min;
    uint64_t next_check;
    uint64_t next_page; /* 0 when the page timer is not armed */
    uint64_t retry_at;
    int frame_fails;

    /* frame download */
    size_t frame_received;
    bool frame_begun;
    bool frame_error;
    const np_http_response_t *frame_resp;

    /* small JSON bodies */
    char body[NP_BODY_MAX];
    size_t body_len;
    bool body_overflow;
} np_app_t;

/* Prepare the state machine. Copies hal and config; the strings in config must stay valid. */
void np_app_init(np_app_t *app, const np_hal_t *hal, const np_app_config_t *config);

/*
 * Do whatever is due now. Returns how many milliseconds the caller should
 * wait before the next call (0 means call again right away). A button press
 * should end the wait early. app->may_sleep says whether a deep sleep with
 * a restart is acceptable for this wait.
 */
uint32_t np_app_step(np_app_t *app);

/* Convenience: one step followed by hal.idle for the returned time. */
void np_app_run_once(np_app_t *app);

#ifdef __cplusplus
}
#endif

#endif
