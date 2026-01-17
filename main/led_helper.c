#include "led_helper.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// RGB LED config (WS2812 on GPIO8)
#define LED_GPIO            8
#define LED_FLASH_MS        50  // flash duration in milliseconds

static const char *TAG = "LED_HELPER";
static led_strip_handle_t s_led_strip = NULL;

// Initialize the LED strip
esp_err_t led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    led_strip_clear(s_led_strip);
    ESP_LOGI(TAG, "RGB LED initialised on GPIO %d", LED_GPIO);

    return ESP_OK;
}

// Set LED to startup color (blue)
void led_set_startup(void)
{
    if (s_led_strip == NULL) return;

    led_strip_set_pixel(s_led_strip, 0, 0, 0, 32);  // Blue (G=0, R=0, B=32)
    led_strip_refresh(s_led_strip);
    ESP_LOGW(TAG, "led_set_startup: LED set to blue");
}

// Flash LED when receiving from UART
void led_flash_rx(void)
{
    if (s_led_strip == NULL) return;

    led_strip_set_pixel(s_led_strip, 0, 32, 0, 0);  // Red (G=32, R=0, B=0)
    led_strip_refresh(s_led_strip);
    vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
    led_strip_clear(s_led_strip);
    led_strip_refresh(s_led_strip);
}

// Flash LED when transmitting to UART
void led_flash_tx(void)
{
    if (s_led_strip == NULL) return;

    led_strip_set_pixel(s_led_strip, 0, 0, 32, 0);  // Green (G=0, R=32, B=0)
    led_strip_refresh(s_led_strip);
    vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
    led_strip_clear(s_led_strip);
    led_strip_refresh(s_led_strip);
}

// Set LED to unconfigured state (purple)
void led_set_unconfigured(void)
{
    if (s_led_strip == NULL) return;

    led_strip_set_pixel(s_led_strip, 0, 0, 32, 32);  // Purple (G=0, R=32, B=32)
    led_strip_refresh(s_led_strip);
    ESP_LOGW(TAG, "led_set_unconfigured: LED set to purple");
}

// Set LED to connected state (yellow)
void led_set_connected(void)
{
    if (s_led_strip == NULL) return;

    led_strip_set_pixel(s_led_strip, 0, 32, 32, 0);  // Yellow (G=32, R=32, B=0)
    led_strip_refresh(s_led_strip);
    ESP_LOGW(TAG, "led_set_connected: LED set to Yellow");
}
