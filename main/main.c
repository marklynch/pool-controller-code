#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "led_helper.h"
#include "mqtt_poolclient.h"
#include "pool_state.h"
#include "wifi_provisioning.h"
#include "tcp_bridge.h"
#include "message_decoder.h"

// ==================== USER CONFIG =====================

#define TCP_PORT        7373

// UART/bus config
#define BUS_UART_NUM    UART_NUM_1
#define BUS_BAUD_RATE   9600        // change to match bus
#define BUS_TX_GPIO     2           // GPIO2 -> NPN base (via 10k)
#define BUS_RX_GPIO     1           // GPIO1 <- divider tap

// UART buffer sizes
#define UART_RX_BUF_SIZE    (2048)
#define UART_TX_BUF_SIZE    (2048)

static const char *TAG = "POOL_BUS_BRIDGE";

// ======================================================
// Pool state (structs defined in pool_state.h)
// ======================================================

pool_state_t s_pool_state = {0};
SemaphoreHandle_t s_pool_state_mutex = NULL;

// ======================================================
// Message decoder wrapper for tcp_bridge callback
// ======================================================

// Decoder context (shared across callbacks)
static message_decoder_context_t s_decoder_context = {
    .pool_state = &s_pool_state,
    .state_mutex = NULL,  // Will be initialized in app_main
    .enable_mqtt = true,
};

// Wrapper for tcp_bridge callback
static bool decode_wrapper_for_bridge(const uint8_t *data, int len)
{
    return decode_message(data, len, &s_decoder_context);
}

// ======================================================
// Send message to bus
// ======================================================

/**
 * Parse a hex string and send the bytes to the bus UART.
 *
 * Accepts hex strings in formats like:
 *   "02 00 50 FF FF 80 00 12 0D 03"
 *   "02005FFFFF8000120D03"
 *   "02 00 50 ff ff 80 00 12 0d 03"
 *
 * Returns the number of bytes sent, or -1 on parse error.
 */
static int bus_send_message(const char *hex_string)
{
    if (hex_string == NULL || hex_string[0] == '\0') {
        ESP_LOGE(TAG, "bus_send_message: empty hex string");
        return -1;
    }

    uint8_t msg_buf[256];
    int msg_len = 0;
    const char *p = hex_string;

    // Parse hex string into bytes
    while (*p != '\0' && msg_len < (int)sizeof(msg_buf)) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') break;

        // Parse two hex digits
        char hex_byte[3] = {0};
        if (*p != '\0') {
            hex_byte[0] = *p++;
        } else {
            ESP_LOGE(TAG, "bus_send_message: incomplete hex byte at position %d", msg_len);
            return -1;
        }
        if (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            hex_byte[1] = *p++;
        } else {
            ESP_LOGE(TAG, "bus_send_message: incomplete hex byte at position %d", msg_len);
            return -1;
        }

        // Convert to byte
        char *endptr;
        unsigned long val = strtoul(hex_byte, &endptr, 16);
        if (endptr != hex_byte + 2) {
            ESP_LOGE(TAG, "bus_send_message: invalid hex at position %d ('%c%c')",
                     msg_len, hex_byte[0], hex_byte[1]);
            return -1;
        }

        msg_buf[msg_len++] = (uint8_t)val;
    }

    if (msg_len == 0) {
        ESP_LOGE(TAG, "bus_send_message: no bytes parsed");
        return -1;
    }

    // Format message for logging
    char hex_log[3 * sizeof(msg_buf) + 4];
    int pos = 0;
    for (int i = 0; i < msg_len; i++) {
        int remaining = sizeof(hex_log) - pos;
        if (remaining < 4) {
            // Not enough space for "XX " + null terminator, truncate
            break;
        }
        int written = snprintf(&hex_log[pos], remaining, "%02X ", msg_buf[i]);
        if (written < 0) {
            // Encoding error (unlikely)
            ESP_LOGE(TAG, "bus_send_message: snprintf encoding error");
            break;
        }
        pos += written;
        // Sanity check: ensure pos doesn't exceed buffer bounds
        if (pos >= (int)sizeof(hex_log)) {
            pos = sizeof(hex_log) - 1;
            break;
        }
    }
    // Ensure null termination within bounds
    if (pos >= (int)sizeof(hex_log)) {
        pos = sizeof(hex_log) - 1;
    }
    hex_log[pos] = '\0';

    ESP_LOGI(TAG, "TX: %s(%d bytes)", hex_log, msg_len);

    // Send to UART
    int written = uart_write_bytes(BUS_UART_NUM, (const char *)msg_buf, msg_len);
    if (written < 0) {
        ESP_LOGE(TAG, "bus_send_message: uart_write_bytes failed");
        return -1;
    }

    // Wait for TX to complete
    ESP_ERROR_CHECK(uart_wait_tx_done(BUS_UART_NUM, pdMS_TO_TICKS(100)));

    ESP_LOGI(TAG, "TX complete: %d bytes written and transmitted", written);

    led_flash_tx();

    return written;
}

// ======================================================
// UART (bus) init
// ======================================================

static void uart_bus_init(void)
{
    // Initialize GPIO2 (TX) to low before UART takes over
    // This ensures the NPN transistor is OFF during initialization
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUS_TX_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(BUS_TX_GPIO, 0));  // Set low
    ESP_LOGI(TAG, "GPIO%d initialized to LOW", BUS_TX_GPIO);

    uart_config_t uart_config = {
        .baud_rate = BUS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(BUS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(BUS_UART_NUM,
                                 BUS_TX_GPIO,
                                 BUS_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    // Install UART driver
    ESP_ERROR_CHECK(uart_driver_install(BUS_UART_NUM,
                                        UART_RX_BUF_SIZE,
                                        UART_TX_BUF_SIZE,
                                        0,
                                        NULL,
                                        0));

    // Invert TX so:
    //   - UART peripheral TX is inverted once,
    //   - transistor inverts again,
    //   => bus sees normal idle-high UART.
    ESP_ERROR_CHECK(uart_set_line_inverse(BUS_UART_NUM, UART_SIGNAL_TXD_INV));

    ESP_LOGI(TAG, "Bus UART initialised: UART%d, RX=%d, TX=%d, baud=%d",
             BUS_UART_NUM, BUS_RX_GPIO, BUS_TX_GPIO, BUS_BAUD_RATE);
}

// ======================================================
// TCP bridge callback wrappers
// ======================================================

/**
 * Wrapper for UART read callback used by TCP bridge
 */
static int uart_read_wrapper(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    return uart_read_bytes(BUS_UART_NUM, buffer, max_len, timeout_ms / portTICK_PERIOD_MS);
}

/**
 * Wrapper for UART write callback used by TCP bridge
 */
static int uart_write_wrapper(const char *hex_string)
{
    return bus_send_message(hex_string);
}

/**
 * Wrapper for message decoder callback used by TCP bridge
 */
static bool decode_wrapper(const uint8_t *data, int len)
{
    return decode_message(data, len, &s_decoder_context);
}

// ======================================================
// app_main
// ======================================================

void app_main(void)
{
    // Init NVS (required for Wi-Fi and provisioning)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Check if running from OTA partition and mark as valid
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA update - marking app as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    ESP_LOGI(TAG, "Running from partition: %s (type %d, subtype %d) at offset 0x%lx",
             running->label, running->type, running->subtype, running->address);

    // Initialize pool state mutex
    s_pool_state_mutex = xSemaphoreCreateMutex();
    if (s_pool_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create pool state mutex");
        return;
    }

    // Initialize decoder context
    s_decoder_context.state_mutex = s_pool_state_mutex;

    // Initialize hardware
    uart_bus_init();
    ESP_ERROR_CHECK(led_init());
    led_set_startup();

    // Initialize WiFi and provisioning
    ESP_LOGI(TAG, "Initializing WiFi and provisioning...");
    ESP_ERROR_CHECK(wifi_provisioning_init());

    // Initialize MQTT client (will start when WiFi connects)
    esp_err_t mqtt_err = mqtt_client_init();
    if (mqtt_err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT client initialized");
    } else if (mqtt_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "MQTT not configured");
    } else {
        ESP_LOGW(TAG, "MQTT initialization failed: %s", esp_err_to_name(mqtt_err));
    }

    // Wait for WiFi connection or stay in provisioning mode
    wifi_wait_for_connection();

    // If we get here, WiFi is connected and HTTP server is running
    ESP_LOGI(TAG, "Starting TCP bridge server...");

    // Start TCP bridge server
    tcp_bridge_config_t bridge_config = {
        .port = TCP_PORT,
        .uart_read = uart_read_wrapper,
        .uart_write = uart_write_wrapper,
        .decode_message = decode_wrapper,
        .led_flash_rx = led_flash_rx,
        .led_flash_tx = led_flash_tx,
    };
    esp_err_t bridge_err = tcp_bridge_start(&bridge_config);
    if (bridge_err == ESP_OK) {
        ESP_LOGI(TAG, "TCP bridge started on port %d", TCP_PORT);
        ESP_LOGI(TAG, "Device IP: %s", wifi_get_device_ip());
    } else {
        ESP_LOGE(TAG, "Failed to start TCP bridge: %s", esp_err_to_name(bridge_err));
    }
}
