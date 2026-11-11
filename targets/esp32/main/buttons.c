/*
 * Three buttons (Next, Back, Home), each between a GPIO and GND with the
 * internal pull-up. A small task samples them every 10 ms, debounces, and
 * queues presses. The buttons also wake the chip from deep sleep; the press
 * that caused the wake is queued at start.
 */
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "platform.h"
#include "sdkconfig.h"

#define COUNT 3
#define SAMPLE_MS 10

static const char *TAG = "buttons";
static const int PINS[COUNT] = { CONFIG_NP_PIN_BTN_NEXT, CONFIG_NP_PIN_BTN_BACK, CONFIG_NP_PIN_BTN_HOME };
static const np_button_t IDS[COUNT] = { NP_BTN_NEXT, NP_BTN_BACK, NP_BTN_HOME };
static QueueHandle_t queue;

static void sample_task(void *arg)
{
    bool pressed[COUNT];
    int stable_for[COUNT] = { 0 };
    /* Start from the current levels: a button still held from the wake is not a second press. */
    for (int i = 0; i < COUNT; i++) {
        pressed[i] = gpio_get_level(PINS[i]) == 0;
    }
    for (;;) {
        for (int i = 0; i < COUNT; i++) {
            bool down = gpio_get_level(PINS[i]) == 0;
            if (down == pressed[i]) {
                stable_for[i] = 0;
                continue;
            }
            stable_for[i] += SAMPLE_MS;
            if (stable_for[i] >= CONFIG_NP_BTN_DEBOUNCE_MS) {
                pressed[i] = down;
                stable_for[i] = 0;
                if (down) {
                    np_button_t id = IDS[i];
                    xQueueSend(queue, &id, 0);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

void buttons_init(void)
{
    queue = xQueueCreate(8, sizeof(np_button_t));
    uint64_t mask = 0;
    for (int i = 0; i < COUNT; i++) {
        if (rtc_gpio_is_valid_gpio(PINS[i])) {
            rtc_gpio_deinit(PINS[i]); /* hand the pin back from the RTC domain after a deep sleep */
        }
        mask |= 1ULL << PINS[i];
    }
    gpio_config_t cfg = { .pin_bit_mask = mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t woke = esp_sleep_get_ext1_wakeup_status();
        for (int i = 0; i < COUNT; i++) {
            if (woke & (1ULL << PINS[i])) {
                np_button_t id = IDS[i];
                xQueueSend(queue, &id, 0);
                ESP_LOGI(TAG, "woken by button on GPIO %d", PINS[i]);
            }
        }
    }
    xTaskCreate(sample_task, "buttons", 2560, NULL, 5, NULL);
}

np_button_t buttons_poll(void *ctx)
{
    np_button_t id;
    return xQueueReceive(queue, &id, 0) == pdTRUE ? id : NP_BTN_NONE;
}

bool buttons_pending(void)
{
    return uxQueueMessagesWaiting(queue) > 0;
}

void buttons_wait(uint32_t ms)
{
    np_button_t id;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    xQueuePeek(queue, &id, ticks ? ticks : 1);
}

bool buttons_home_held(void)
{
    return gpio_get_level(CONFIG_NP_PIN_BTN_HOME) == 0;
}

void buttons_prepare_sleep(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < COUNT; i++) {
        if (!rtc_gpio_is_valid_gpio(PINS[i])) {
            ESP_LOGW(TAG, "GPIO %d cannot wake the chip from deep sleep", PINS[i]);
            continue;
        }
        /* The digital pull-up is off in deep sleep; use the RTC one. */
        rtc_gpio_init(PINS[i]);
        rtc_gpio_set_direction(PINS[i], RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_dis(PINS[i]);
        rtc_gpio_pullup_en(PINS[i]);
        mask |= 1ULL << PINS[i];
    }
    if (mask) {
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON); /* keeps the pull-ups powered */
        esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    }
}
