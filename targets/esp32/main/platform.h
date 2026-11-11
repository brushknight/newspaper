/* ESP32 side of the HAL: the pieces main.c assembles into np_hal_t. */
#ifndef NP_PLATFORM_H
#define NP_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "el133uf1.h"
#include "hal.h"

/* panel_bus.c: SPI and GPIO wiring of the panel. */
void panel_bus_init(el133uf1_bus_t *bus);
/* Keep the control lines at their idle levels through deep sleep. */
void panel_bus_prepare_sleep(void);

/* storage.c: NVS for durable keys, RTC memory for "rt_" keys. */
void storage_init(void);
bool storage_get(void *ctx, const char *key, char *buf, size_t cap);
bool storage_set(void *ctx, const char *key, const char *value);
void storage_factory_reset(void);

/* buttons.c */
void buttons_init(void);
np_button_t buttons_poll(void *ctx);
/* Block until a press is queued or the time is over. */
void buttons_wait(uint32_t ms);
bool buttons_pending(void);
bool buttons_home_held(void);
/* Arm the buttons as deep sleep wake sources. */
void buttons_prepare_sleep(void);

/* net.c: Wi-Fi station and HTTP client. */
void net_init(void);
bool net_has_credentials(void *ctx);
bool net_connect(void *ctx);
int net_rssi(void *ctx);
void net_stop(void);
int net_http_request(void *ctx, const np_http_request_t *req, np_http_body_cb on_body, void *user,
                     np_http_response_t *resp);

/* provisioning.c: setup access point with a captive portal. */
bool provisioning_start(void *ctx, np_prov_info_t *info);
bool provisioning_done(void *ctx);
void provisioning_stop(void *ctx);

/* NVS keys owned by the target, not by the core. */
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_WIFI_OK "wifi_ok" /* "1" once these credentials have worked */

#endif
