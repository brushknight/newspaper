/*
 * Key/value storage for the core.
 *
 * Durable keys (token, Wi-Fi, what is on the panel) live in NVS flash.
 * Keys that start with "rt_" are timers the core wants back after a deep
 * sleep; they change often, so they live in RTC memory, which survives
 * sleep, is lost on power loss, and has no wear.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "platform.h"

#define NAMESPACE "newspaper"
#define RT_SLOTS 8

typedef struct {
    char key[16];
    char value[24];
} rt_slot_t;

static RTC_DATA_ATTR rt_slot_t rt_slots[RT_SLOTS];

static const char *TAG = "storage";
static nvs_handle_t nvs;

void storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(NAMESPACE, NVS_READWRITE, &nvs));
}

static bool is_rt(const char *key)
{
    return strncmp(key, "rt_", 3) == 0;
}

bool storage_get(void *ctx, const char *key, char *buf, size_t cap)
{
    if (is_rt(key)) {
        for (int i = 0; i < RT_SLOTS; i++) {
            if (rt_slots[i].key[0] && strcmp(rt_slots[i].key, key) == 0) {
                if (strlen(rt_slots[i].value) + 1 > cap) {
                    return false;
                }
                strcpy(buf, rt_slots[i].value);
                return true;
            }
        }
        return false;
    }
    size_t len = cap;
    return nvs_get_str(nvs, key, buf, &len) == ESP_OK;
}

bool storage_set(void *ctx, const char *key, const char *value)
{
    if (is_rt(key)) {
        int slot = -1;
        for (int i = 0; i < RT_SLOTS; i++) {
            if (rt_slots[i].key[0] && strcmp(rt_slots[i].key, key) == 0) {
                slot = i;
                break;
            }
            if (!rt_slots[i].key[0] && slot < 0) {
                slot = i;
            }
        }
        if (slot < 0 || strlen(key) >= sizeof(rt_slots[0].key)) {
            return false;
        }
        if (!value) {
            rt_slots[slot].key[0] = 0;
            return true;
        }
        if (strlen(value) >= sizeof(rt_slots[0].value)) {
            return false;
        }
        strcpy(rt_slots[slot].key, key);
        strcpy(rt_slots[slot].value, value);
        return true;
    }
    esp_err_t err;
    if (value) {
        err = nvs_set_str(nvs, key, value);
    } else {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

void storage_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset: erasing Wi-Fi credentials and pairing");
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    memset(rt_slots, 0, sizeof(rt_slots));
}
