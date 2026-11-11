/*
 * The panel's pins on the ESP32: SPI2 for clock and data, plain GPIOs for
 * the two chip selects, D/C, RESET and BUSY. This is the bus the portable
 * driver in drivers/el133uf1 runs on.
 *
 * UNTESTED ON HARDWARE, like the driver itself.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform.h"
#include "sdkconfig.h"

#define PIN_SCLK CONFIG_NP_PIN_PANEL_SCLK
#define PIN_MOSI CONFIG_NP_PIN_PANEL_MOSI
#define PIN_CS0 CONFIG_NP_PIN_PANEL_CS0
#define PIN_CS1 CONFIG_NP_PIN_PANEL_CS1
#define PIN_DC CONFIG_NP_PIN_PANEL_DC
#define PIN_RST CONFIG_NP_PIN_PANEL_RST
#define PIN_BUSY CONFIG_NP_PIN_PANEL_BUSY
#define PIN_POWER CONFIG_NP_PIN_PANEL_POWER

#define CHUNK 4096

static const char *TAG = "panel";
static spi_device_handle_t spi;
static uint8_t *dma_buf; /* SPI with DMA needs buffers from DMA capable memory */

static void bus_set_reset(void *ctx, int level)
{
    gpio_set_level(PIN_RST, level);
}

static void bus_set_cs(void *ctx, unsigned mask)
{
    /* Active low: a selected controller's line goes low. */
    gpio_set_level(PIN_CS0, (mask & EL133UF1_CS0) ? 0 : 1);
    gpio_set_level(PIN_CS1, (mask & EL133UF1_CS1) ? 0 : 1);
}

static void bus_set_dc(void *ctx, int level)
{
    gpio_set_level(PIN_DC, level);
}

static void bus_write(void *ctx, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t n = len > CHUNK ? CHUNK : len;
        memcpy(dma_buf, data, n);
        spi_transaction_t t = { .length = n * 8, .tx_buffer = dma_buf };
        esp_err_t err = spi_device_polling_transmit(spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi transmit failed: %s", esp_err_to_name(err));
            return;
        }
        data += n;
        len -= n;
    }
}

static int bus_read_busy(void *ctx)
{
    return gpio_get_level(PIN_BUSY);
}

static void bus_delay(void *ctx, uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

void panel_bus_init(el133uf1_bus_t *bus)
{
    /* Release the levels held through a deep sleep (see panel_bus_prepare_sleep). */
    gpio_hold_dis(PIN_CS0);
    gpio_hold_dis(PIN_CS1);
    gpio_hold_dis(PIN_RST);
    gpio_deep_sleep_hold_dis();

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_CS0) | (1ULL << PIN_CS1) | (1ULL << PIN_DC) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(PIN_CS0, 1);
    gpio_set_level(PIN_CS1, 1);
    gpio_set_level(PIN_RST, 1);
    gpio_set_level(PIN_DC, 0);
    ESP_ERROR_CHECK(gpio_config(&out));

    /* Pull-up as in the Pimoroni driver: a disconnected panel then reads "ready", not "busy forever". */
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

#if CONFIG_NP_PIN_PANEL_POWER >= 0
    gpio_config_t pwr = { .pin_bit_mask = 1ULL << PIN_POWER, .mode = GPIO_MODE_OUTPUT };
    gpio_set_level(PIN_POWER, 1);
    ESP_ERROR_CHECK(gpio_config(&pwr));
#endif

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = CONFIG_NP_PANEL_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1, /* the driver moves the two chip selects itself */
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi));
    dma_buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA);
    assert(dma_buf);

    bus->ctx = NULL;
    bus->set_reset = bus_set_reset;
    bus->set_cs = bus_set_cs;
    bus->set_dc = bus_set_dc;
    bus->write = bus_write;
    bus->read_busy = bus_read_busy;
    bus->delay_ms = bus_delay;
    ESP_LOGI(TAG, "EL133UF1 on SCLK %d MOSI %d CS0 %d CS1 %d DC %d RST %d BUSY %d (driver untested on hardware)",
             PIN_SCLK, PIN_MOSI, PIN_CS0, PIN_CS1, PIN_DC, PIN_RST, PIN_BUSY);
}

void panel_bus_prepare_sleep(void)
{
    /* Floating chip selects or reset could disturb the panel while the chip sleeps. */
    gpio_set_level(PIN_CS0, 1);
    gpio_set_level(PIN_CS1, 1);
    gpio_set_level(PIN_RST, 1);
    gpio_hold_en(PIN_CS0);
    gpio_hold_en(PIN_CS1);
    gpio_hold_en(PIN_RST);
    gpio_deep_sleep_hold_en();
}
