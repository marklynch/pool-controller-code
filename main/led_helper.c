#include "led_helper.h"
#include "config.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "LED_HELPER";
static led_strip_handle_t s_led_strip = NULL;

// State memory for restoring after flashes
typedef struct {
    uint8_t green;
    uint8_t red;
    uint8_t blue;
} led_rgb_t;

static led_rgb_t s_current_state = {0, 0, 0};
static led_persistent_state_t s_persistent_state = LED_STATE_STARTUP;
static SemaphoreHandle_t s_led_mutex = NULL;

// Helper function to set LED and save state
static void set_led_state(uint8_t green, uint8_t red, uint8_t blue, led_persistent_state_t state)
{
    if (s_led_strip == NULL) return;

    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_current_state.green = green;
        s_current_state.red = red;
        s_current_state.blue = blue;
        s_persistent_state = state;
        led_strip_set_pixel(s_led_strip, 0, green, red, blue);
        led_strip_refresh(s_led_strip);
        xSemaphoreGive(s_led_mutex);
    }
}

// Initialize the LED strip
esp_err_t led_init(void)
{
    s_led_mutex = xSemaphoreCreateMutex();
    if (s_led_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create LED mutex");
        return ESP_ERR_NO_MEM;
    }

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
    set_led_state(0, 0, 32, LED_STATE_STARTUP);  // Blue
    ESP_LOGI(TAG, "LED: Startup (Blue)");
}

// Flash LED when receiving from UART (cyan flash)
void led_flash_rx(void)
{
    if (s_led_strip == NULL) return;

    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        led_strip_set_pixel(s_led_strip, 0, 32, 0, 32);  // Cyan (G=32, R=0, B=32)
        led_strip_refresh(s_led_strip);
        xSemaphoreGive(s_led_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(LED_FLASH_DURATION_MS));

    // Restore current state — read after the delay so we pick up any set_led_state()
    // call that happened during the flash, rather than overwriting it with a stale snapshot
    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        led_strip_set_pixel(s_led_strip, 0, s_current_state.green, s_current_state.red, s_current_state.blue);
        led_strip_refresh(s_led_strip);
        xSemaphoreGive(s_led_mutex);
    }
}

// Flash LED when transmitting to UART (magenta flash)
void led_flash_tx(void)
{
    if (s_led_strip == NULL) return;

    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        led_strip_set_pixel(s_led_strip, 0, 0, 32, 32);  // Magenta (G=0, R=32, B=32)
        led_strip_refresh(s_led_strip);
        xSemaphoreGive(s_led_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(LED_FLASH_DURATION_MS));

    // Restore current state — read after the delay so we pick up any set_led_state()
    // call that happened during the flash, rather than overwriting it with a stale snapshot
    if (xSemaphoreTake(s_led_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        led_strip_set_pixel(s_led_strip, 0, s_current_state.green, s_current_state.red, s_current_state.blue);
        led_strip_refresh(s_led_strip);
        xSemaphoreGive(s_led_mutex);
    }
}

// Set LED to unconfigured state (purple)
void led_set_unconfigured(void)
{
    set_led_state(0, 32, 32, LED_STATE_UNCONFIGURED);  // Purple
    ESP_LOGI(TAG, "LED: Unconfigured - WiFi setup needed (Purple)");
}

// Set LED to WiFi connected state (white) - MQTT not yet connected
void led_set_connected(void)
{
    set_led_state(20, 20, 20, LED_STATE_WIFI_ONLY);  // White (neutral, partial connection)
    ESP_LOGI(TAG, "LED: WiFi connected, waiting for MQTT (White)");
}

// Set LED to fully connected state (green) - WiFi + MQTT both operational
void led_set_mqtt_connected(void)
{
    set_led_state(32, 0, 0, LED_STATE_FULLY_CONNECTED);  // Green (universal "all good")
    ESP_LOGI(TAG, "LED: Fully connected - WiFi + MQTT operational (Green)");
}

// Set LED to MQTT disconnected state (orange) - WiFi ok, MQTT issue
void led_set_mqtt_disconnected(void)
{
    set_led_state(16, 32, 0, LED_STATE_MQTT_DISCONNECTED);  // Orange (warning state)
    ESP_LOGW(TAG, "LED: MQTT disconnected - WiFi ok, MQTT issue (Orange)");
}
