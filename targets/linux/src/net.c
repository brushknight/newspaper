/*
 * Network side of the HAL on Linux: HTTPS through libcurl, and the check
 * that the network is up before the core makes a request.
 *
 * The system owns Wi-Fi (NetworkManager on Raspberry Pi OS, wpa_supplicant
 * on the flashable image), so "connect" here only waits, for a while, for a
 * default route and a clock that has been set. TLS certificate checks need
 * the right date, and a Raspberry Pi has no battery backed clock.
 *
 * Response bodies are handed to the core piece by piece, so the 960 kB
 * frame streams into the panel and never sits in memory.
 */
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

#ifndef NP_FIRMWARE_VERSION
#define NP_FIRMWARE_VERSION "0.0.0-dev"
#endif

/* 2025-01-01. A clock before this has not been set since boot. */
#define PLAUSIBLE_EPOCH 1735689600

static const np_linux_config_t *cfg;
static CURL *curl;
static char ca_file[NP_PATH_MAX];
static char user_agent[96];

int net_init(const np_linux_config_t *config)
{
    cfg = config;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        return -1;
    }
    curl = curl_easy_init();
    if (!curl) {
        return -1;
    }
    snprintf(user_agent, sizeof(user_agent), "morning-newspaper-display/%s (linux)", NP_FIRMWARE_VERSION);
    if (cfg->ca_file[0]) {
        snprintf(ca_file, sizeof(ca_file), "%s", cfg->ca_file);
    }
#ifdef NP_STATIC
    else {
        /* A static build carries no idea of where this system keeps its CA bundle; look in the usual places. */
        static const char *candidates[] = {
            "/etc/ssl/certs/ca-certificates.crt", /* Debian, Raspberry Pi OS, Buildroot */
            "/etc/ssl/cert.pem",                  /* Alpine, macOS */
            "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora */
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (access(candidates[i], R_OK) == 0) {
                snprintf(ca_file, sizeof(ca_file), "%s", candidates[i]);
                break;
            }
        }
    }
#endif
    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    np_log(NP_LOG_INFO, "net: libcurl %s, %s%s%s", v->version, v->ssl_version ? v->ssl_version : "no TLS",
           ca_file[0] ? ", CA bundle " : "", ca_file);
    return 0;
}

/* ---- readiness ---- */

static char *read_text(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

typedef enum { NET_NO_ROUTE, NET_NO_CLOCK, NET_READY, NET_UNKNOWN } net_state_t;

static net_state_t net_state(void)
{
    static char route[8192], route6[8192];
    char *r4 = read_text("/proc/net/route", route, sizeof(route));
    char *r6 = read_text("/proc/net/ipv6_route", route6, sizeof(route6));
    if (!r4 && !r6) {
        return NET_UNKNOWN; /* not Linux, or no /proc: just try */
    }
    if (!np_net_has_default_route(r4, r6)) {
        return NET_NO_ROUTE;
    }
    return time(NULL) < PLAUSIBLE_EPOCH ? NET_NO_CLOCK : NET_READY;
}

bool net_wait_ready(void *ctx)
{
    if (!cfg->network_check) {
        return true;
    }
    net_state_t s = net_state();
    bool told = false;
    for (uint32_t waited = 0; s != NET_READY && s != NET_UNKNOWN && waited < cfg->network_wait_s; waited++) {
        if (!told) {
            np_log(NP_LOG_INFO, s == NET_NO_ROUTE ? "net: waiting for the network (no default route)"
                                                  : "net: waiting for the clock to be set (NTP)");
            told = true;
        }
        buttons_wait(1000);
        if (np_stop_requested()) {
            return false;
        }
        s = net_state();
    }
    if (s == NET_NO_ROUTE) {
        np_log(NP_LOG_WARN, "net: no network after %u s", (unsigned)cfg->network_wait_s);
        return false;
    }
    if (s == NET_NO_CLOCK) {
        /* There is a route; let the request try. TLS will say if the date is the problem. */
        np_log(NP_LOG_WARN, "net: the clock still looks unset; HTTPS may fail until it is");
    }
    return true;
}

int net_rssi(void *ctx)
{
    char buf[2048];
    return np_net_wireless_dbm(read_text("/proc/net/wireless", buf, sizeof(buf)));
}

/* ---- http ---- */

typedef struct {
    np_http_body_cb on_body;
    void *user;
    np_http_response_t *resp;
    bool started;
    bool aborted;
} xfer_t;

static size_t on_header(char *data, size_t size, size_t nitems, void *p)
{
    xfer_t *x = p;
    size_t len = size * nitems;
    if (len > 5 && strncmp(data, "HTTP/", 5) == 0) {
        x->resp->retry_after_s = -1; /* a new response (after a redirect or 100 Continue) */
    } else if (len > 12 && strncasecmp(data, "Retry-After:", 12) == 0) {
        int s = atoi(data + 12); /* the HTTP date form is not supported and reads as 0 */
        x->resp->retry_after_s = s > 0 ? s : -1;
    }
    return len;
}

static size_t on_write(char *data, size_t size, size_t nmemb, void *p)
{
    xfer_t *x = p;
    size_t len = size * nmemb;
    if (!x->started) {
        /* The core relies on the status being known before the first body piece. */
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        x->resp->status = (int)code;
        x->started = true;
    }
    if (!x->on_body(x->user, (const uint8_t *)data, len)) {
        x->aborted = true;
        return 0;
    }
    return len;
}

static int on_progress(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    return np_stop_requested() ? 1 : 0;
}

int net_http_request(void *ctx, const np_http_request_t *req, np_http_body_cb on_body, void *user,
                     np_http_response_t *resp)
{
    xfer_t x = { .on_body = on_body, .user = user, .resp = resp };
    resp->status = 0;
    resp->retry_after_s = -1;

    curl_easy_reset(curl); /* keeps the connection cache */
    struct curl_slist *headers = NULL;
    for (int i = 0; i < req->header_count; i++) {
        char line[512];
        snprintf(line, sizeof(line), "%s: %s", req->headers[i].name, req->headers[i].value);
        headers = curl_slist_append(headers, line);
    }
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &x);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (ca_file[0]) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    }
    if (strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body ? req->body : "");
    }

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) {
        if (!x.aborted) {
            np_log(NP_LOG_WARN, "net: %s %s: %s", req->method, req->url, curl_easy_strerror(rc));
        }
        return -1;
    }
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    resp->status = (int)code;
    return 0;
}
