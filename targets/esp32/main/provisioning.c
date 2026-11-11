/*
 * Wi-Fi provisioning: an open access point "Newspaper-XXXX" with a captive
 * portal. The panel shows a Wi-Fi QR code for the access point (drawn by
 * the core). The phone joins, the portal page opens, the owner picks the
 * home network and types the password. The credentials go to NVS.
 *
 * Captive portal = a DNS server that answers every name with our address,
 * plus an HTTP server that redirects every unknown URL to the setup page.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "platform.h"
#include "sdkconfig.h"

#define PORTAL_IP "192.168.4.1"
#define MAX_NETWORKS 16

static const char *TAG = "setup";
static httpd_handle_t server;
static volatile bool dns_run;
static volatile bool done;
static char networks[MAX_NETWORKS][33];
static int network_count;

/* ---- DNS: every name resolves to the portal ---- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns: cannot bind port 53");
        if (sock >= 0) {
            close(sock);
        }
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t pkt[512];
    while (dns_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, pkt, sizeof(pkt) - 16, 0, (struct sockaddr *)&from, &from_len);
        if (n < 17 || (pkt[2] & 0x80) || pkt[4] != 0 || pkt[5] != 1) {
            continue; /* timeout, not a query, or not exactly one question */
        }
        /* Find the end of the question: labels, a zero byte, type and class. */
        int p = 12;
        while (p < n && pkt[p] != 0) {
            p += pkt[p] + 1;
        }
        p += 5;
        if (p > n) {
            continue;
        }
        pkt[2] = 0x84; /* response, authoritative */
        pkt[3] = 0x00;
        pkt[6] = 0;
        pkt[7] = 1; /* one answer */
        pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0;
        static const uint8_t answer[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
        memcpy(pkt + p, answer, sizeof(answer));
        sendto(sock, pkt, p + sizeof(answer), 0, (struct sockaddr *)&from, from_len);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ---- HTTP ---- */

static size_t html_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 7 < cap; in++) {
        const char *rep = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '"' ? "&quot;" : NULL;
        if (rep) {
            n += (size_t)snprintf(out + n, cap - n, "%s", rep);
        } else {
            out[n++] = *in;
        }
    }
    out[n] = 0;
    return n;
}

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=\"en\"><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Newspaper display setup</title>"
    "<style>body{font:17px system-ui,sans-serif;max-width:26rem;margin:2rem auto;padding:0 1rem;color:#1d1c1a}"
    "h1{font-size:1.4rem}label{display:block;margin:1rem 0 .3rem}"
    "input,select,button{font:inherit;width:100%;padding:.6rem;box-sizing:border-box}"
    "button{margin-top:1.4rem;background:#d9641e;color:#fff;border:0;border-radius:6px}</style>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>The Morning Newspaper</h1><p>Connect this display to your Wi-Fi network.</p>"
                                  "<form method=\"post\" action=\"/save\"><label for=\"s\">Network</label>");
    if (network_count > 0) {
        httpd_resp_sendstr_chunk(req, "<select id=\"s\" name=\"ssid\">");
        for (int i = 0; i < network_count; i++) {
            char esc[33 * 6];
            char opt[sizeof(esc) + 24];
            html_escape(networks[i], esc, sizeof(esc));
            snprintf(opt, sizeof(opt), "<option>%s</option>", esc);
            httpd_resp_sendstr_chunk(req, opt);
        }
        httpd_resp_sendstr_chunk(req, "</select><label for=\"o\">or type another network name</label>"
                                      "<input id=\"o\" name=\"other\" maxlength=\"32\" autocapitalize=\"none\">");
    } else {
        httpd_resp_sendstr_chunk(req, "<input id=\"s\" name=\"ssid\" maxlength=\"32\" autocapitalize=\"none\" required>");
    }
    httpd_resp_sendstr_chunk(req, "<label for=\"p\">Password</label>"
                                  "<input id=\"p\" name=\"pass\" type=\"password\" maxlength=\"64\">"
                                  "<button>Save and connect</button></form></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t redirect_get(httpd_req_t *req)
{
    /* Phones probe well known URLs to detect a captive portal. Send them all to the setup page. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Copy one field of an application/x-www-form-urlencoded body, decoded. */
static bool form_field(const char *body, const char *name, char *out, size_t cap)
{
    size_t name_len = strlen(name);
    const char *p = body;
    out[0] = 0;
    while (p && *p) {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            size_t n = 0;
            while (*p && *p != '&' && n + 1 < cap) {
                if (*p == '+') {
                    out[n++] = ' ';
                    p++;
                } else if (*p == '%' && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
                    out[n++] = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
                    p += 3;
                } else {
                    out[n++] = *p++;
                }
            }
            out[n] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return false;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[512];
    if (req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    body[got] = 0;

    char ssid[33], other[33], pass[65];
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "other", other, sizeof(other));
    form_field(body, "pass", pass, sizeof(pass));
    const char *chosen = other[0] ? other : ssid;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (!chosen[0]) {
        httpd_resp_sendstr_chunk(req, PAGE_HEAD);
        httpd_resp_sendstr_chunk(req, "<h1>No network chosen</h1><p><a href=\"/\">Back</a></p></html>");
        return httpd_resp_sendstr_chunk(req, NULL);
    }
    storage_set(NULL, KEY_WIFI_SSID, chosen);
    storage_set(NULL, KEY_WIFI_PASS, pass);
    storage_set(NULL, KEY_WIFI_OK, NULL);
    ESP_LOGI(TAG, "credentials saved for \"%s\"", chosen);
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>Saved</h1><p>The display now joins your network and this setup network "
                                  "disappears. Look at the display: it shows a code to add it to your account. "
                                  "If the Wi-Fi setup screen comes back, the password was wrong.</p></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    done = true;
    return ESP_OK;
}

/* ---- start and stop ---- */

static void scan_networks(void)
{
    network_count = 0;
    wifi_scan_config_t scan = { 0 };
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        return;
    }
    uint16_t count = 24;
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (!records || esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        free(records);
        return;
    }
    for (int i = 0; i < count && network_count < MAX_NETWORKS; i++) {
        const char *name = (const char *)records[i].ssid;
        bool seen = name[0] == 0;
        for (int j = 0; j < network_count && !seen; j++) {
            seen = strcmp(networks[j], name) == 0;
        }
        if (!seen) {
            snprintf(networks[network_count++], sizeof(networks[0]), "%s", name);
        }
    }
    free(records);
    ESP_LOGI(TAG, "found %d networks", network_count);
}

bool provisioning_start(void *ctx, np_prov_info_t *info)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(info->ssid, sizeof(info->ssid), "%s-%02X%02X", CONFIG_NP_SETUP_AP_PREFIX, mac[4], mac[5]);
    info->password[0] = 0; /* open network: it exists for a few minutes and carries one form */
    snprintf(info->url, sizeof(info->url), "http://" PORTAL_IP);

    net_stop();
    wifi_config_t ap = { .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
    ap.ap.ssid_len = (uint8_t)snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", info->ssid);
    /* Access point plus station, so nearby networks can be listed on the setup page. */
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        return false;
    }
    scan_networks();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        esp_wifi_stop();
        return false;
    }
    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = page_get };
    static const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    static const httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = redirect_get };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &any);

    done = false;
    dns_run = true;
    xTaskCreate(dns_task, "setup_dns", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "access point \"%s\" is up, portal at http://" PORTAL_IP, info->ssid);
    return true;
}

bool provisioning_done(void *ctx)
{
    return done;
}

void provisioning_stop(void *ctx)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let the "Saved" page reach the phone */
    dns_run = false;
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(1200)); /* the DNS task notices within a second */
    esp_wifi_stop();
}
