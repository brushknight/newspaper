/*
 * The Morning Newspaper display, ESP32-S3 firmware.
 *
 * This file only assembles the HAL and runs the portable core (core/app.c).
 * What the display does is decided there; how it reaches the hardware is in
 * the other files of this directory.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "app.h"
#include "el133uf1.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform.h"
#include "sdkconfig.h"

static const char *TAG = "newspaper";
static el133uf1_t panel;
static np_app_t app;

/* ---- display: the portable driver on the SPI bus ---- */

static int display_begin(void *ctx)
{
    return el133uf1_begin_frame(&panel);
}

static int display_write(void *ctx, int half, const uint8_t *data, size_t len)
{
    return el133uf1_write(&panel, half, data, len);
}

static int display_refresh(void *ctx)
{
    return el133uf1_refresh(&panel);
}

static bool display_busy(void *ctx)
{
    return el133uf1_busy(&panel);
}

static void display_power_off(void *ctx)
{
    el133uf1_power_off(&panel);
}

static uint32_t display_min_interval(void *ctx)
{
    return EL133UF1_MIN_REFRESH_INTERVAL_MS;
}

/* ---- clock and sleep ---- */

/*
 * The system time is never set from the network, so it simply counts from
 * power-on. ESP-IDF keeps it running through deep sleep on the RTC timer,
 * which is exactly the clock the core asks for.
 */
static uint64_t now_ms(void *ctx)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

static void idle(void *ctx, uint32_t ms, bool may_power_down)
{
#if CONFIG_NP_DEEP_SLEEP
    if (may_power_down && ms >= CONFIG_NP_DEEP_SLEEP_MIN_SECONDS * 1000u && !buttons_pending()) {
        ESP_LOGI(TAG, "deep sleep for %u s (buttons wake)", (unsigned)(ms / 1000u));
        net_stop();
        panel_bus_prepare_sleep();
        buttons_prepare_sleep();
        esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000u);
        esp_deep_sleep_start(); /* does not return: the chip restarts and the core restores its state */
    }
#endif
    buttons_wait(ms);
}

static void log_line(void *ctx, np_log_level_t level, const char *message)
{
    switch (level) {
    case NP_LOG_ERROR: ESP_LOGE(TAG, "%s", message); break;
    case NP_LOG_WARN: ESP_LOGW(TAG, "%s", message); break;
    case NP_LOG_INFO: ESP_LOGI(TAG, "%s", message); break;
    default: ESP_LOGD(TAG, "%s", message); break;
    }
}

/* Mains power is assumed for now; a battery gauge would report its percentage here. */
static int battery_percent(void *ctx)
{
    return -1;
}

void app_main(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    bool cold_start = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED;
    ESP_LOGI(TAG, "The Morning Newspaper display, firmware %s", desc->version);

    storage_init();
    buttons_init();

    /* Factory reset: hold Home while plugging the display in, and keep holding for five seconds. */
    if (cold_start && buttons_home_held()) {
        ESP_LOGW(TAG, "Home is held: keep holding for 5 s to erase Wi-Fi and pairing");
        int held_ms = 0;
        while (buttons_home_held() && held_ms < 5000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            held_ms += 100;
        }
        if (held_ms >= 5000) {
            storage_factory_reset();
        }
        while (buttons_poll(NULL) != NP_BTN_NONE) {
        }
    }

    el133uf1_bus_t bus;
    panel_bus_init(&bus);
    el133uf1_init(&panel, &bus);
    net_init();

    static char hardware_id[24];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hardware_id, sizeof(hardware_id), "esp32-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);

    np_hal_t hal = {
        .display_begin_frame = display_begin,
        .display_write = display_write,
        .display_refresh = display_refresh,
        .display_busy = display_busy,
        .display_power_off = display_power_off,
        .display_min_refresh_interval_ms = display_min_interval,
        .http_request = net_http_request,
        .storage_get = storage_get,
        .storage_set = storage_set,
        .now_ms = now_ms,
        .idle = idle,
        .button_poll = buttons_poll,
        .wifi_has_credentials = net_has_credentials,
        .wifi_connect = net_connect,
        .wifi_rssi = net_rssi,
        .prov_start = provisioning_start,
        .prov_done = provisioning_done,
        .prov_stop = provisioning_stop,
        .battery_percent = battery_percent,
        .log = log_line,
    };
    np_app_config_t config = {
        .api_base = CONFIG_NP_API_BASE_URL,
        .hardware_id = hardware_id,
        .firmware = desc->version,
        .emulator = false,
    };
    np_app_init(&app, &hal, &config);
    ESP_LOGI(TAG, "%s, platform %s", hardware_id, CONFIG_NP_API_BASE_URL);

    for (;;) {
        np_app_run_once(&app);
    }
}
