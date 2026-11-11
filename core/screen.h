/*
 * Setup and status screens drawn by the device itself.
 *
 * A screen is rendered row by row straight into the el133uf1 frame format
 * and handed to a sink in small pieces: first all of half 0 (left 600
 * columns), then all of half 1. No full frame buffer is ever needed.
 */
#ifndef NP_SCREEN_H
#define NP_SCREEN_H

#include <stddef.h>
#include <stdint.h>

/* Receives consecutive pieces of a half. Return 0 to continue, non-zero to stop. */
typedef int (*np_frame_sink_t)(void *user, int half, const uint8_t *data, size_t len);

typedef struct {
    const char *qr_text;     /* verification_uri_complete */
    const char *display_url; /* verification_uri, shown as text */
    const char *user_code;   /* for example K7QF-3MXD */
    const char *footer;      /* small print, for example hardware id and firmware */
} np_pair_view_t;

typedef struct {
    const char *ssid;     /* setup access point name */
    const char *password; /* "" for an open access point */
    const char *url;      /* setup page address */
    const char *footer;
} np_prov_view_t;

#define NP_MESSAGE_MAX_LINES 5

typedef struct {
    const char *title;
    const char *lines[NP_MESSAGE_MAX_LINES]; /* unused entries NULL */
    const char *footer;
} np_message_view_t;

/* Each returns 0 on success, or the sink's non-zero value, or -1 on bad input. */
int np_screen_pairing(const np_pair_view_t *view, np_frame_sink_t sink, void *user);
int np_screen_provisioning(const np_prov_view_t *view, np_frame_sink_t sink, void *user);
int np_screen_message(const np_message_view_t *view, np_frame_sink_t sink, void *user);

#endif
