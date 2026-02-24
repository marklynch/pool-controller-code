#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_netif_sntp.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "config.h"
#include "led_helper.h"
#include "mqtt_poolclient.h"
#include "pool_state.h"
#include "wifi_provisioning.h"
#include "tcp_bridge.h"
#include "message_decoder.h"
#include "register_requester.h"

// ==================== APPLICATION =====================
// All configuration values are in config.h

static const char *TAG = "POOL_BUS_BRIDGE";

// ======================================================
// Pool state (structs defined in pool_state.h)
// ======================================================

pool_state_t s_pool_state = {0};
SemaphoreHandle_t s_pool_state_mutex = NULL;

// ======================================================
// Message decoder wrapper for tcp_bridge callback
// ======================================================

// Decoder context (shared across callbacks and accessible to web handlers)
message_decoder_context_t s_decoder_context = {
    .pool_state = &s_pool_state,
    .state_mutex = NULL,  // Will be initialized in app_main
    .enable_mqtt = true,
};


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
int bus_send_message(const char *hex_string)
{
    if (hex_string == NULL || hex_string[0] == '\0') {
        ESP_LOGE(TAG, "bus_send_message: empty hex string");
        return -1;
    }

    uint8_t msg_buf[BUS_MESSAGE_MAX_SIZE];
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
    esp_err_t tx_err = uart_wait_tx_done(BUS_UART_NUM, pdMS_TO_TICKS(UART_TX_TIMEOUT_MS));
    if (tx_err != ESP_OK) {
        ESP_LOGE(TAG, "uart_wait_tx_done failed: %s", esp_err_to_name(tx_err));
        return -1;
    }

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
                                        UART_RX_BUFFER_SIZE,
                                        UART_TX_BUFFER_SIZE,
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
// Logging Configuration
// ======================================================

/**
 * Configure log levels for application and system components
 *
 * Available log levels (from most to least verbose):
 *   ESP_LOG_NONE    - No log output
 *   ESP_LOG_ERROR   - Critical errors, system may fail
 *   ESP_LOG_WARN    - Error conditions, but system continues
 *   ESP_LOG_INFO    - Informational messages
 *   ESP_LOG_DEBUG   - Extra information for debugging
 *   ESP_LOG_VERBOSE - All details, including data dumps
 */
static void configure_log_levels(void)
{
    // Set global default log level (affects all modules not explicitly configured)
    esp_log_level_set("*", ESP_LOG_INFO);

    // Application components
    esp_log_level_set("POOL_BUS_BRIDGE", ESP_LOG_INFO);    // Main application
    esp_log_level_set("MSG_DECODER", ESP_LOG_INFO);        // Message decoder
    esp_log_level_set("TCP_BRIDGE", ESP_LOG_INFO);         // TCP bridge server
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_WARN);        // MQTT client
    esp_log_level_set("MQTT_PUBLISH", ESP_LOG_WARN);       // MQTT publishing
    esp_log_level_set("MQTT_DISCOVERY", ESP_LOG_WARN);     // MQTT discovery
    esp_log_level_set("MQTT_COMMANDS", ESP_LOG_INFO);      // MQTT commands
    esp_log_level_set("WEB_HANDLERS", ESP_LOG_INFO);       // HTTP handlers
    esp_log_level_set("WIFI_PROV", ESP_LOG_INFO);          // WiFi provisioning
    esp_log_level_set("LED_HELPER", ESP_LOG_INFO);         // LED status

    // System components (ESP-IDF) - uncomment to reduce verbosity
    // esp_log_level_set("wifi", ESP_LOG_WARN);            // WiFi stack
    // esp_log_level_set("httpd", ESP_LOG_WARN);           // HTTP server
    // esp_log_level_set("esp_netif", ESP_LOG_WARN);       // Network interface
}

// ======================================================
// app_main
// ======================================================

void app_main(void)
{
    // Configure logging early to control verbosity during startup
    configure_log_levels();

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

    // Start NTP time sync
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_netif_sntp_init(&sntp_config);
    ESP_LOGI(TAG, "NTP sync started with server: %s", NTP_SERVER);

    ESP_LOGI(TAG, "Starting TCP bridge server...");

    // Start TCP bridge server
    tcp_bridge_config_t bridge_config = {
        .port = TCP_BRIDGE_PORT,
        .uart_read = uart_read_wrapper,
        .uart_write = uart_write_wrapper,
        .decode_message = decode_wrapper,
        .led_flash_rx = led_flash_rx,
        .led_flash_tx = led_flash_tx,
    };
    esp_err_t bridge_err = tcp_bridge_start(&bridge_config);
    if (bridge_err == ESP_OK) {
        ESP_LOGI(TAG, "TCP bridge started on port %d", TCP_BRIDGE_PORT);
        ESP_LOGI(TAG, "Device IP: %s", wifi_get_device_ip());
    } else {
        ESP_LOGE(TAG, "Failed to start TCP bridge: %s", esp_err_to_name(bridge_err));
    }

    register_requester_start();
}
