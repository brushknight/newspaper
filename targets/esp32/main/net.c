/*
 * Network side of the HAL: Wi-Fi station and HTTP(S) client.
 *
 * TLS uses the ESP-IDF root certificate bundle. Response bodies are read in
 * 4 kB pieces and handed straight to the core, so a 960 kB frame never sits
 * in memory.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "platform.h"
#include "sdkconfig.h"

#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1
#define MAX_RETRIES 3

static const char *TAG = "net";
static EventGroupHandle_t events;
static bool wifi_started;
static bool connected;
static bool connecting;
static int retries;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        if (connecting) {
            if (retries++ < MAX_RETRIES) {
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(events, BIT_FAILED);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        xEventGroupSetBits(events, BIT_CONNECTED);
    }
}

void net_init(void)
{
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* credentials are kept by storage.c */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
}

bool net_has_credentials(void *ctx)
{
    char ssid[33];
    return storage_get(NULL, KEY_WIFI_SSID, ssid, sizeof(ssid)) && ssid[0];
}

void net_stop(void)
{
    connecting = false;
    connected = false;
    if (wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        wifi_started = false;
    }
}

bool net_connect(void *ctx)
{
    if (connected) {
        return true;
    }
    wifi_config_t wc = { 0 };
    char pass[65] = "";
    if (!storage_get(NULL, KEY_WIFI_SSID, (char *)wc.sta.ssid, sizeof(wc.sta.ssid))) {
        return false;
    }
    storage_get(NULL, KEY_WIFI_PASS, pass, sizeof(pass));
    memcpy(wc.sta.password, pass, strlen(pass) < sizeof(wc.sta.password) ? strlen(pass) : sizeof(wc.sta.password));

    net_stop();
    xEventGroupClearBits(events, BIT_CONNECTED | BIT_FAILED);
    retries = 0;
    connecting = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started = true;
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(events, BIT_CONNECTED | BIT_FAILED, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_NP_WIFI_CONNECT_TIMEOUT_S * 1000));
    connecting = false;

    char ok[4];
    bool proven = storage_get(NULL, KEY_WIFI_OK, ok, sizeof(ok));
    if (bits & BIT_CONNECTED) {
        ESP_LOGI(TAG, "connected to \"%s\"", (char *)wc.sta.ssid);
        if (!proven) {
            storage_set(NULL, KEY_WIFI_OK, "1");
        }
        return true;
    }
    ESP_LOGW(TAG, "could not connect to \"%s\"", (char *)wc.sta.ssid);
    net_stop();
    if (!proven) {
        /*
         * These credentials have never worked, so they are most likely
         * mistyped. Drop them and go back to the setup access point rather
         * than leave the owner with a display that cannot be reached.
         */
        ESP_LOGW(TAG, "new credentials failed, returning to Wi-Fi setup");
        storage_set(NULL, KEY_WIFI_SSID, NULL);
        storage_set(NULL, KEY_WIFI_PASS, NULL);
        esp_restart();
    }
    return false;
}

int net_rssi(void *ctx)
{
    wifi_ap_record_t ap;
    if (connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

/* ---- http ---- */

typedef struct {
    int retry_after_s;
} http_ctx_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Retry-After") == 0) {
        http_ctx_t *hc = evt->user_data;
        int s = atoi(evt->header_value); /* the HTTP date form is not supported and reads as 0 */
        hc->retry_after_s = s > 0 ? s : -1;
    }
    return ESP_OK;
}

int net_http_request(void *ctx, const np_http_request_t *req, np_http_body_cb on_body, void *user,
                     np_http_response_t *resp)
{
    static uint8_t buf[4096];
    http_ctx_t hc = { .retry_after_s = -1 };
    esp_http_client_config_t cfg = {
        .url = req->url,
        .method = strcmp(req->method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .user_data = &hc,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = "morning-newspaper-display",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }
    for (int i = 0; i < req->header_count; i++) {
        esp_http_client_set_header(client, req->headers[i].name, req->headers[i].value);
    }
    int rc = -1;
    int body_len = req->body ? (int)strlen(req->body) : 0;
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: %s", req->method, req->url, esp_err_to_name(err));
        goto done;
    }
    if (body_len > 0 && esp_http_client_write(client, req->body, body_len) != body_len) {
        goto done;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        goto done;
    }
    /* The core relies on the status being known before the first body piece. */
    resp->status = esp_http_client_get_status_code(client);
    resp->retry_after_s = hc.retry_after_s;
    for (;;) {
        int n = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (n < 0) {
            goto done;
        }
        if (n == 0) {
            if (!esp_http_client_is_complete_data_received(client)) {
                ESP_LOGW(TAG, "%s: connection closed before the body was complete", req->url);
                goto done;
            }
            break;
        }
        if (!on_body(user, buf, (size_t)n)) {
            goto done;
        }
    }
    rc = 0;
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return rc;
}
