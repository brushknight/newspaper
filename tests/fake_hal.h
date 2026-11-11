/*
 * Fake HAL for the host tests: a fake clock, an in-memory key/value store,
 * a panel that records what it is sent and counts rule violations, and a
 * tiny in-process model of the platform from docs/PROTOCOL.md.
 */
#ifndef FAKE_HAL_H
#define FAKE_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "frame.h"

#define FAKE_MAX_KEYS 32
#define FAKE_MAX_LOG 64

typedef struct {
    /* clock */
    uint64_t now;

    /* storage */
    struct {
        char key[16];
        char value[256];
        bool used;
    } kv[FAKE_MAX_KEYS];
    int storage_writes;

    /* panel */
    uint8_t *loading;        /* NP_FRAME_BYTES being written */
    uint8_t *shown;          /* NP_FRAME_BYTES last refreshed */
    size_t half_bytes[2];
    int begin_count;
    int refresh_count;
    int power_off_count;
    bool panel_busy_flag;
    uint64_t busy_until;
    uint64_t refresh_started[FAKE_MAX_LOG];
    uint32_t refresh_ms;         /* how long a refresh takes */
    uint32_t min_interval_ms;
    int violations;              /* refresh while busy, too soon, or with a bad frame; writes out of order */
    size_t max_write;            /* largest single display_write */
    int write_calls;

    /* buttons */
    np_button_t queue[16];
    int queue_len;

    /* wifi */
    bool wifi_creds;
    bool wifi_ok;
    bool prov_started;
    bool prov_finished;
    int prov_start_calls;

    /* platform model */
    bool net_down;
    int start_calls, poll_calls, state_calls, frame_calls;
    uint64_t last_poll_at;
    uint64_t min_poll_gap; /* smallest time seen between two polls */
    int pair_serial;       /* how many codes were issued */
    uint64_t pair_expires_at;
    int pair_expires_in;   /* seconds */
    int pair_interval;     /* seconds */
    bool adopted;
    bool token_given;
    bool token_valid;
    bool issue_present;
    char issue_version[64];
    char issue_date[16];
    int issue_pages;
    int check_interval_min;
    int page_interval_min;
    int frame_503_left;    /* answer 503 this many times */
    size_t frame_truncate; /* if non-zero, send only this many bytes */
    size_t chunk_sizes[4]; /* body delivery pattern, repeated; 0 ends the list */
    int frame_pages[FAKE_MAX_LOG]; /* pages requested, in order */
    char last_body[512];
    bool saw_emulator_flag;
    bool saw_auth_header;
} fake_t;

void fake_init(fake_t *f);
void fake_free(fake_t *f);
np_hal_t fake_hal(fake_t *f);
void fake_press(fake_t *f, np_button_t b);
const char *fake_get(fake_t *f, const char *key);
/* The byte the fake platform sends at `offset` of the frame for `page`. */
uint8_t fake_frame_byte(int page, size_t offset);

#endif
