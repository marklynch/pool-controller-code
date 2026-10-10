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

#include "config.h"
#include "bus.h"
#include "led_helper.h"
#include "mqtt_poolclient.h"
#include "pool_state.h"
#include "wifi_provisioning.h"
#include "tcp_bridge.h"
#include "message_decoder.h"
#include "register_requester.h"
#include "channel_power.h"
#include "filter_pump_type.h"
#include "channel_energy.h"
#include "unknown_buffer.h"
#include "heap_monitor.h"
#include "firmware_update.h"

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
// TCP bridge callback wrappers
// ======================================================

static bool decode_wrapper(const uint8_t *data, int len)
{
    return decode_message(data, len, &s_decoder_context);
}

static void resync_wrapper(tcp_bridge_resync_type_t type, const uint8_t *data, int len)
{
    if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    s_pool_state.resyncs_total++;
    switch (type) {
        case TCP_BRIDGE_RESYNC_NO_START:            s_pool_state.resyncs_no_start++;            break;
        case TCP_BRIDGE_RESYNC_BAD_HEADER_CHECKSUM: s_pool_state.resyncs_bad_header_checksum++; break;
        case TCP_BRIDGE_RESYNC_BAD_FRAME_TYPE:      s_pool_state.resyncs_bad_frame_type++;      break;
        case TCP_BRIDGE_RESYNC_BAD_LENGTH:          s_pool_state.resyncs_bad_length++;          break;
        case TCP_BRIDGE_RESYNC_BAD_END:             s_pool_state.resyncs_bad_end++;             break;
        case TCP_BRIDGE_RESYNC_BAD_DATA_CHECKSUM:   s_pool_state.resyncs_bad_data_checksum++;   break;
        case TCP_BRIDGE_RESYNC_BUFFER_OVERFLOW:     s_pool_state.resyncs_buffer_overflow++;     break;
        default:                                    s_pool_state.resyncs_unexpected++;          break;
    }
    xSemaphoreGive(s_pool_state_mutex);

    if (data && len > 0) {
        unknown_reason_t reason;
        switch (type) {
            case TCP_BRIDGE_RESYNC_NO_START:            reason = UNKNOWN_REASON_NO_START;        break;
            case TCP_BRIDGE_RESYNC_BAD_HEADER_CHECKSUM: reason = UNKNOWN_REASON_HEADER_CHECKSUM;  break;
            case TCP_BRIDGE_RESYNC_BAD_FRAME_TYPE:      reason = UNKNOWN_REASON_BAD_FRAME_TYPE;   break;
            case TCP_BRIDGE_RESYNC_BAD_LENGTH:          reason = UNKNOWN_REASON_BAD_LENGTH;       break;
            case TCP_BRIDGE_RESYNC_BAD_END:             reason = UNKNOWN_REASON_BAD_END;          break;
            case TCP_BRIDGE_RESYNC_BAD_DATA_CHECKSUM:   reason = UNKNOWN_REASON_DATA_CHECKSUM;    break;
            case TCP_BRIDGE_RESYNC_BUFFER_OVERFLOW:     reason = UNKNOWN_REASON_BUFFER_OVERFLOW;  break;
            default:                                    reason = UNKNOWN_REASON_UNEXPECTED;       break;
        }
        unknown_buffer_record(data, len, reason);
    }
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

    // Load per-channel configured power (NVS) before the decoder/publisher
    // can run and needs to resolve effective channel power
    channel_power_init();
    filter_pump_type_init();

    // Initialize hardware
    unknown_buffer_init();
    bus_init();
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

    // Initialize the GitHub firmware-update checker. Done before WiFi connects
    // so its state/locks exist by the time MQTT connects and publishes the HA
    // update entity's state; the check task itself waits for the network.
    firmware_update_init();

    // Give the network a brief chance to come up so the startup log can name
    // the device's address, but never block on it: the bus bridge and the
    // local services must run whether or not WiFi is available, and the
    // station keeps retrying in the background either way.
    if (wifi_wait_for_connection(WIFI_STARTUP_WAIT_MS)) {
        ESP_LOGI(TAG, "WiFi connected - device IP: %s", wifi_get_device_ip());
    } else {
        ESP_LOGW(TAG, "WiFi not up yet - continuing startup without it");
    }

    // Start NTP time sync
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_netif_sntp_init(&sntp_config);
    ESP_LOGI(TAG, "NTP sync started with server: %s", NTP_SERVER);

    ESP_LOGI(TAG, "Starting TCP bridge server...");

    // Start TCP bridge server
    tcp_bridge_config_t bridge_config = {
        .port = TCP_BRIDGE_PORT,
        .uart_read = bus_read,
        .uart_write = bus_send_message,
        .decode_message = decode_wrapper,
        .led_flash_rx = led_flash_rx,
        .led_flash_tx = led_flash_tx,
        .on_resync = resync_wrapper,
    };
    esp_err_t bridge_err = tcp_bridge_start(&bridge_config);
    if (bridge_err == ESP_OK) {
        ESP_LOGI(TAG, "TCP bridge started on port %d", TCP_BRIDGE_PORT);
    } else {
        ESP_LOGE(TAG, "Failed to start TCP bridge: %s", esp_err_to_name(bridge_err));
    }

    register_requester_start(&s_pool_state, s_pool_state_mutex);

    // Periodically integrate each channel's effective power into a
    // cumulative kWh total and publish it as an HA energy sensor.
    channel_energy_start();

    // Periodically log heap stats so a slow leak or fragmentation trend is
    // visible in the log history before it can exhaust memory and crash.
    heap_monitor_start();
}
