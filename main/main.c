#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include "lwip/netdb.h"
#include "lwip/err.h"

#include <esp_http_server.h>
#include "nvs.h"
#include "led_helper.h"
#include "mqtt_poolclient.h"
#include "mqtt_publish.h"
#include "pool_state.h"
#include "web_handlers.h"
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

// Wi-Fi event bits
#define WIFI_CONNECTED_BIT  BIT0


static const char *TAG = "POOL_BUS_BRIDGE";

static EventGroupHandle_t s_wifi_event_group;

// Provisioning state
static bool s_provisioning_active = false;
static bool s_wifi_connected = false;
static httpd_handle_t s_httpd_handle = NULL;
static int s_wifi_retry_count = 0;
static TimerHandle_t s_wifi_retry_timer = NULL;
static char s_device_ip_address[16] = {0};  // Stores current IP address (xxx.xxx.xxx.xxx)

#define WIFI_MAX_RETRY 5  // After 5 failed attempts, clear credentials and restart
#define WIFI_RETRY_DELAY_MS 5000  // 5 second delay between retry attempts

// ======================================================
// Pool state (structs defined in pool_state.h)
// ======================================================

pool_state_t s_pool_state = {0};
SemaphoreHandle_t s_pool_state_mutex = NULL;

// ======================================================
// Wi-Fi retry timer callback
// ======================================================

static void wifi_retry_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Retry timer expired, attempting reconnection (attempt %d/%d)...",
             s_wifi_retry_count, WIFI_MAX_RETRY);
    esp_wifi_connect();
}

// ======================================================
// Wi-Fi event handler
// ======================================================

// Clear WiFi credentials from NVS
static esp_err_t wifi_credentials_clear(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_key(nvs_handle, PROV_NVS_KEY_SSID);
    nvs_erase_key(nvs_handle, PROV_NVS_KEY_PASS);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
    return ESP_OK;
}


static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_device_ip_address[0] = '\0';  // Clear IP address
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Stop MQTT client on WiFi disconnect
        mqtt_client_stop();

        if (!s_provisioning_active) {
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected (attempt %d/%d)",
                     s_wifi_retry_count, WIFI_MAX_RETRY);

            if (s_wifi_retry_count >= WIFI_MAX_RETRY) {
                ESP_LOGE(TAG, "WiFi connection failed after %d attempts - clearing credentials and restarting",
                         WIFI_MAX_RETRY);
                wifi_credentials_clear();
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                // Start retry timer for delayed reconnection
                ESP_LOGI(TAG, "Will retry connection in %d seconds...", WIFI_RETRY_DELAY_MS / 1000);
                if (s_wifi_retry_timer != NULL) {
                    xTimerStart(s_wifi_retry_timer, 0);
                }
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        // Store IP address for logging/reference
        snprintf(s_device_ip_address, sizeof(s_device_ip_address),
                 IPSTR, IP2STR(&event->ip_info.ip));

        ESP_LOGI(TAG, "Got IP: %s", s_device_ip_address);
        s_wifi_connected = true;
        s_wifi_retry_count = 0;  // Reset retry counter on successful connection

        // Stop retry timer if running
        if (s_wifi_retry_timer != NULL && xTimerIsTimerActive(s_wifi_retry_timer)) {
            xTimerStop(s_wifi_retry_timer, 0);
        }

        led_set_connected();  // Set green LED when connected
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Start MQTT client on WiFi connect
        mqtt_client_start();
    }
}

// ======================================================
// WiFi Provisioning Helper Functions
// ======================================================

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             PROV_SOFTAP_SSID_PREFIX, eth_mac[3], eth_mac[4], eth_mac[5]);
}

// Check if WiFi credentials exist in NVS
static bool wifi_credentials_exist(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, PROV_NVS_KEY_SSID, NULL, &required_size);
    nvs_close(nvs_handle);

    return (err == ESP_OK && required_size > 0);
}

// Load WiFi credentials from NVS
static esp_err_t wifi_credentials_load(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;

    err = nvs_get_str(nvs_handle, PROV_NVS_KEY_SSID, ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs_handle, PROV_NVS_KEY_PASS, password, &pass_size);
    }

    nvs_close(nvs_handle);
    return err;
}

// Save WiFi credentials to NVS
esp_err_t wifi_credentials_save(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, PROV_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, PROV_NVS_KEY_PASS, password);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}




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
// Wi-Fi station init
// ======================================================

static void wifi_init_sta(void)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    // Create WiFi retry timer (one-shot timer)
    if (s_wifi_retry_timer == NULL) {
        s_wifi_retry_timer = xTimerCreate("wifi_retry",
                                          pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS),
                                          pdFALSE,  // One-shot timer
                                          NULL,
                                          wifi_retry_timer_callback);
        if (s_wifi_retry_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create WiFi retry timer");
        }
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();  // Needed for provisioning

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_LOGI(TAG, "WiFi initialization complete");
}

// ======================================================
// HTTP Server Start (unified for provisioning and normal mode)
// ======================================================

static esp_err_t start_http_server(const char *ip_address)
{
    if (s_httpd_handle != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = 80;
    httpd_config.max_uri_handlers = 12;   // Increased from default 8 to handle all endpoints
    httpd_config.recv_wait_timeout = 60;  // 60 seconds for large file uploads
    httpd_config.send_wait_timeout = 60;  // 60 seconds for responses
    httpd_config.stack_size = 8192;       // Larger stack for OTA operations

    esp_err_t err = httpd_start(&s_httpd_handle, &httpd_config);
    if (err == ESP_OK) {
        web_handlers_register(s_httpd_handle);
        ESP_LOGI(TAG, "HTTP server started at http://%s", ip_address);
        ESP_LOGI(TAG, "  - WiFi config: http://%s/", ip_address);
        ESP_LOGI(TAG, "  - Pool status: http://%s/status", ip_address);
        ESP_LOGI(TAG, "  - MQTT config: http://%s/mqtt_config", ip_address);
        ESP_LOGI(TAG, "  - Firmware update: http://%s/update", ip_address);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
    }

    return err;
}

// ======================================================
// WiFi Provisioning Start
// ======================================================

static esp_err_t start_provisioning(void)
{
    // Check if WiFi credentials exist in NVS
    if (wifi_credentials_exist()) {
        ESP_LOGI(TAG, "WiFi credentials found in NVS, connecting...");

        // Load credentials from NVS
        char ssid[33] = {0};
        char password[64] = {0};
        esp_err_t err = wifi_credentials_load(ssid, sizeof(ssid), password, sizeof(password));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load credentials from NVS: %s", esp_err_to_name(err));
            return err;
        }

        // Configure WiFi with loaded credentials
        wifi_config_t wifi_cfg = {0};
        memcpy(wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
        memcpy(wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
        return ESP_OK;
    }

    // No credentials found - start provisioning mode
    ESP_LOGI(TAG, "No WiFi credentials found, starting provisioning mode");
    s_provisioning_active = true;

    // Set purple LED for unconfigured WiFi
    led_set_unconfigured();

    // Generate AP SSID
    char ap_ssid[32];
    get_device_service_name(ap_ssid, sizeof(ap_ssid));

    // Configure SoftAP
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    memcpy(wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started - SSID: %s", ap_ssid);

    // Start HTTP server for web provisioning
    return start_http_server("192.168.4.1");
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
    led_set_startup();  // Show led on startup

    // Check WiFi configuration state and set LED color
    if (!wifi_credentials_exist()) {
        led_set_unconfigured();  // Purple LED for unconfigured WiFi
        ESP_LOGI(TAG, "WiFi unconfigured - LED set to purple");
    }

    // Initialize WiFi and start provisioning
    wifi_init_sta();
    ESP_ERROR_CHECK(start_provisioning());

    // Initialize MQTT client (will start when WiFi connects)
    esp_err_t mqtt_err = mqtt_client_init();
    if (mqtt_err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT client initialized");
    } else if (mqtt_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "MQTT not configured");
    } else {
        ESP_LOGW(TAG, "MQTT initialization failed: %s", esp_err_to_name(mqtt_err));
    }

    if (s_provisioning_active) {
        ESP_LOGI(TAG, "Provisioning mode active - waiting for configuration...");
        ESP_LOGI(TAG, "Connect to the WiFi AP and navigate to http://192.168.4.1");
        // Wait indefinitely in provisioning mode (will restart after provisioning)
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    } else {
        // Wait for WiFi connection
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_CONNECTED_BIT,
                            pdFALSE, pdFALSE,
                            portMAX_DELAY);

        ESP_LOGI(TAG, "WiFi connected, starting HTTP server and TCP server...");

        // Start HTTP server for status API
        start_http_server(s_device_ip_address);

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
        } else {
            ESP_LOGE(TAG, "Failed to start TCP bridge: %s", esp_err_to_name(bridge_err));
        }
    }
}