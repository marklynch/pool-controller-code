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
// Message decoder
// ======================================================

// Address Subsystem
// 50 Main Controller (Connect 10)
// 62 Temperature sensor
// 90 Chemistry (pH, ORP, Chlorinator)
// 6F Touch Screen (dest only) ????
// F0 Internet Gateway

// Assumption of message structure is:
// 02 Start Byte
// 00 50 - Source Address (example: 50 = Main Controller)
// FF FF - Destination Address (FF FF = Broadcast)
// 80 00 Control Byte or Flags
// 12 - command 1
// 0D - command 2
// 2F - command 3
// 01 - data bytes (variable length) 
// 01 - value or checksum component - sum of all data bytes (low byte)
// 03 End Byte

// Message type patterns (messages start with 0x02, end with 0x03)
// 50 Main Controller (Connect 10)
static const uint8_t MSG_TYPE_REGISTER_STATUS[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x0F, 0x17};  // Register Status
static const uint8_t MSG_TYPE_REGISTER_LABEL[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x1A, 0x22};  // Register labels (favourites, etc.)
static const uint8_t MSG_TYPE_38_BASE[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38};  // Shorter pattern for channel names
static const uint8_t MSG_TYPE_TEMP_SETTING[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x17, 0x10};
static const uint8_t MSG_TYPE_CONFIG[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x26, 0x0E};
static const uint8_t MSG_TYPE_MODE[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x14, 0x0D, 0xF1};
static const uint8_t MSG_TYPE_CHANNELS[] = {0x02, 0x00, 0x50, 0x00, 0x6F, 0x80, 0x00, 0x0D, 0x0D, 0x5B};
static const uint8_t MSG_TYPE_CHANNEL_STATUS[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x0B, 0x25, 0x00};
static const uint8_t MSG_TYPE_LIGHT_CONFIG[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x06, 0x0E, 0xE4};

// 62 Temperature sensor / Unknown subsystem
static const uint8_t MSG_TYPE_TEMP_READING[] = {0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00, 0x16, 0x0E};
static const uint8_t MSG_TYPE_HEATER[] = {0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00, 0x12, 0x0F};

// 90 Chlorinator (pH, ORP)
static const uint8_t MSG_TYPE_CHLOR[] = {0x02, 0x00, 0x90, 0xFF, 0xFF, 0x80, 0x00};

// Chlorinator sub-type patterns (bytes 7-10)
static const uint8_t CHLOR_PH_SETPOINT[]  = {0x1D, 0x0F, 0x3C, 0x01};
static const uint8_t CHLOR_ORP_SETPOINT[] = {0x1D, 0x0F, 0x3C, 0x02};
static const uint8_t CHLOR_PH_READING[]   = {0x1F, 0x0F, 0x3E, 0x01};
static const uint8_t CHLOR_ORP_READING[]  = {0x1F, 0x0F, 0x3E, 0x02};


// 6F Touch Screen (source only)

// F0 Internet Gateway
static const uint8_t MSG_TYPE_SERIAL_NUMBER[] = {0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00, 0x37, 0x11};


// Channel type names
const char *CHANNEL_TYPE_NAMES[] = {
    "Unknown",       // 0
    "Filter",        // 1
    "Cleaning",      // 2
    "Heater Pump",   // 3
    "Booster",       // 4
    "Waterfall",     // 5
    "Fountain",      // 6
    "Spa Pump",      // 7
    "Solar",         // 8
    "Blower",        // 9
    "Swimjet",       // 10
    "Jets",          // 11
    "Spa Jets",      // 12
    "Overflow",      // 13
    "Spillway",      // 14
    "Audio",         // 15
    "Hot Seat",      // 16
    "Heater Power",  // 17
    "Custom Name",   // 18
};
#define CHANNEL_TYPE_COUNT 19
#define CHANNEL_UNUSED 0xFE
#define CHANNEL_END    0xFD

// Channel state names
const char *CHANNEL_STATE_NAMES[] = {
    "Off",          // 0
    "Auto",         // 1
    "On",           // 2
    "Low Speed",    // 3
    "Medium Speed", // 4
    "High Speed",   // 5
};
#define CHANNEL_STATE_COUNT 6

// Lighting state names
const char *LIGHTING_STATE_NAMES[] = {
    "Off",          // 0
    "Auto",         // 1
    "On",           // 2
};
#define LIGHTING_STATE_COUNT 3

// Lighting color names
const char *LIGHTING_COLOR_NAMES[] = {
    "Unknown",           // 0
    "Red",               // 1
    "Orange",            // 2
    "Yellow",            // 3
    "Green",             // 4
    "Blue",              // 5
    "Purple",            // 6
    "White",             // 7
    "User 1",            // 8
    "User 2",            // 9
    "Disco",             // 10
    "Smooth",            // 11
    "Fade",              // 12
    "Magenta",           // 13
    "Cyan",              // 14
    "Pattern",           // 15
    "Rainbow",           // 16
    "Ocean",             // 17
    "Voodoo Lounge",     // 18
    "Deep Blue Sea",     // 19
    "Royal Blue",        // 20
    "Afternoon Skies",   // 21
    "Aqua Green",        // 22
    "Emerald",           // 23
    "Warm Red",          // 24
    "Flamingo",          // 25
    "Vivid Violet",      // 26
    "Sangria",           // 27
    "Twilight",          // 28
    "Tranquillity",      // 29
    "Gemstone",          // 30
    "USA",               // 31
    "Mardi Gras",        // 32
    "Cool Cabaret",      // 33
    "Sam",               // 34
    "Party",             // 35
    "Romance",           // 36
    "Caribbean",         // 37
    "American",          // 38
    "California Sunset", // 39
    "Royal",             // 40
    "Hold",              // 41
    "Recall",            // 42
    "Peruvian Paradise", // 43
    "Super Nova",        // 44
    "Northern Lights",   // 45
    "Tidal Wave",        // 46
    "Patriot Dream",     // 47
    "Desert Skies",      // 48
    "Nova",              // 49
    "Pink",              // 50
};
#define LIGHTING_COLOR_COUNT 51

// Device address lookup
static const char* get_device_name(uint8_t addr_hi, uint8_t addr_lo) {
    if (addr_hi == 0xFF && addr_lo == 0xFF) return "Broadcast";
    if (addr_hi == 0x00) {
        switch (addr_lo) {
            case 0x50: return "Controller";
            case 0x62: return "Temp Sensor";
            case 0x90: return "Chlorinator";
            case 0x6F: return "Touch Screen";
            case 0xF0: return "Internet GW";
        }
    }
    return NULL;
}

// Forward declaration for checksum verification
bool verify_message_checksum(const uint8_t *data, int len);

// Returns true if message was decoded, false otherwise
static bool decode_message(const uint8_t *data, int len)
{
    // Must start with 0x02 and end with 0x03
    if (len < 7 || data[0] != 0x02 || data[len - 1] != 0x03) {
        return false;
    }

    // Verify checksum if message is long enough
    if (len >= 13) {
        if (!verify_message_checksum(data, len)) {
            // Format full message as hex string
            char hexMsg[3 * len + 1];
            int pos = 0;
            for (int i = 0; i < len; i++) {
                pos += snprintf(&hexMsg[pos], sizeof(hexMsg) - pos, "%02X ", data[i]);
            }
            hexMsg[pos] = '\0';

            // Calculate what we expected (sum from index 10 to len-3)
            uint32_t sum = 0;
            for (int i = 10; i < len - 2; i++) {
                sum += data[i];
            }
            uint8_t calculated = sum & 0xFF;
            uint8_t received = data[len - 2];

            ESP_LOGW(TAG, "Checksum FAILED: %s", hexMsg);
            ESP_LOGW(TAG, "  Calculated: 0x%02X, Received: 0x%02X", calculated, received);
        } else {
            ESP_LOGD(TAG, "Checksum verification OK");
        }
    }

    // Extract source and destination addresses
    uint8_t src_hi = data[1], src_lo = data[2];
    uint8_t dst_hi = data[3], dst_lo = data[4];
    const char *src_name = get_device_name(src_hi, src_lo);
    const char *dst_name = get_device_name(dst_hi, dst_lo);

    // Log addresses (format depends on whether names are known)
    char addr_info[64];
    if (src_name && dst_name) {
        snprintf(addr_info, sizeof(addr_info), "[%s -> %s]", src_name, dst_name);
    } else if (src_name) {
        snprintf(addr_info, sizeof(addr_info), "[%s -> %02X%02X]", src_name, dst_hi, dst_lo);
    } else if (dst_name) {
        snprintf(addr_info, sizeof(addr_info), "[%02X%02X -> %s]", src_hi, src_lo, dst_name);
    } else {
        snprintf(addr_info, sizeof(addr_info), "[%02X%02X -> %02X%02X]", src_hi, src_lo, dst_hi, dst_lo);
    }

    // Register label messages (byte 10 = register ID, byte 12+ = label string)
    if (len >= sizeof(MSG_TYPE_REGISTER_LABEL) + 3 && memcmp(data, MSG_TYPE_REGISTER_LABEL, sizeof(MSG_TYPE_REGISTER_LABEL)) == 0) {
        uint8_t reg_id = data[10];
        // Label string starts at byte 12, null-terminated
        const char *label = (const char *)&data[12];

        ESP_LOGI(TAG, "%s Register 0x%02X label - \"%s\"", addr_info, reg_id, label);

        // Update pool state - find or create entry for this register
        if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Find existing entry or first available slot
            int slot = -1;
            for (int i = 0; i < 32; i++) {
                if (s_pool_state.register_labels[i].valid && s_pool_state.register_labels[i].reg_id == reg_id) {
                    slot = i;  // Found existing entry
                    break;
                } else if (!s_pool_state.register_labels[i].valid && slot == -1) {
                    slot = i;  // Remember first available slot
                }
            }

            if (slot >= 0) {
                s_pool_state.register_labels[slot].reg_id = reg_id;
                strncpy(s_pool_state.register_labels[slot].label, label, sizeof(s_pool_state.register_labels[slot].label) - 1);
                s_pool_state.register_labels[slot].label[sizeof(s_pool_state.register_labels[slot].label) - 1] = '\0';
                s_pool_state.register_labels[slot].valid = true;
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            }

            xSemaphoreGive(s_pool_state_mutex);
        }
        return true;
    }

    // Channel name messages (byte 10 = 0x7C-0x83 for channels 1-8)
    if (len >= sizeof(MSG_TYPE_38_BASE) + 5 && memcmp(data, MSG_TYPE_38_BASE, sizeof(MSG_TYPE_38_BASE)) == 0) {
        uint8_t ch_id = data[10];
        if (ch_id >= 0x7C && ch_id <= 0x83) {
            uint8_t ch_num = ch_id - 0x7C + 1;
            // Name starts at byte 12, null-terminated
            const char *name = (const char *)&data[12];
            // Check if it's an empty/unused channel (first byte is 0x00)
            if (data[12] == 0x00) {
                ESP_LOGI(TAG, "%s Channel %d name - (empty)", addr_info, ch_num);
            } else {
                ESP_LOGI(TAG, "%s Channel %d name - \"%s\"", addr_info, ch_num, name);

                // Update pool state
                if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (ch_num <= 8) {
                        strncpy(s_pool_state.channels[ch_num - 1].name, name, sizeof(s_pool_state.channels[ch_num - 1].name) - 1);
                        s_pool_state.channels[ch_num - 1].id = ch_num;
                        s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    }
                    xSemaphoreGive(s_pool_state_mutex);
                }
            }
            return true;
        }
    }

    // Lighting zone configuration messages - tells us which zones are actually installed
    if (len >= sizeof(MSG_TYPE_LIGHT_CONFIG) + 4 && memcmp(data, MSG_TYPE_LIGHT_CONFIG, sizeof(MSG_TYPE_LIGHT_CONFIG)) == 0) {
        uint8_t zone_idx = data[10];
        if (zone_idx <= 3) {
            // Mark this zone as configured (only publish if this is the first time)
            if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                bool was_configured = s_pool_state.lighting[zone_idx].configured;

                s_pool_state.lighting[zone_idx].zone = zone_idx + 1;
                s_pool_state.lighting[zone_idx].configured = true;
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Only log and publish if this is the first time we're marking it as configured
                if (!was_configured) {
                    ESP_LOGI(TAG, "%s Lighting zone %d configured", addr_info, zone_idx + 1);
                    // Publish MQTT discovery for this zone now that we know it's configured
                    mqtt_publish_light(&s_pool_state, zone_idx + 1);
                }

                xSemaphoreGive(s_pool_state_mutex);
            }
        }
        return true;
    }

    if (len >= sizeof(MSG_TYPE_REGISTER_STATUS) && memcmp(data, MSG_TYPE_REGISTER_STATUS, sizeof(MSG_TYPE_REGISTER_STATUS)) == 0) {
        
        uint8_t channel = data[10];
        uint8_t value1 = data[11];
        uint8_t value2 = data[12];

        if (channel >= 0xC0 && channel <= 0xC3) {
            // Lighting zone state
            const char *state = (value2 < LIGHTING_STATE_COUNT) ? LIGHTING_STATE_NAMES[value2] : "Unknown";
            ESP_LOGI(TAG, "%s Lighting zone %d - %s", addr_info, channel - 0xC0 + 1, state);

            // Update pool state (only publish if zone is already configured)
            if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint8_t zone_idx = channel - 0xC0;
                s_pool_state.lighting[zone_idx].zone = zone_idx + 1;
                s_pool_state.lighting[zone_idx].state = value2;
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Only publish if this zone was configured via MSG_TYPE_LIGHT_CONFIG
                if (s_pool_state.lighting[zone_idx].configured) {
                    mqtt_publish_light(&s_pool_state, s_pool_state.lighting[zone_idx].zone);
                }

                xSemaphoreGive(s_pool_state_mutex);
            }
        } else if (channel >= 0xD0 && channel <= 0xD3) {
            // Lighting zone color (D0-D3 = zones 1-4)
            uint8_t zone = channel - 0xD0 + 1;
            uint8_t color = value2;
            const char *color_name = (color < LIGHTING_COLOR_COUNT) ? LIGHTING_COLOR_NAMES[color] : "Unknown";
            ESP_LOGI(TAG, "%s Lighting zone %d color - %s (%d)", addr_info, zone, color_name, color);

            // Update pool state (only publish if zone is already configured)
            if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint8_t zone_idx = channel - 0xD0;
                s_pool_state.lighting[zone_idx].color = color;
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Only publish if this zone was configured via MSG_TYPE_LIGHT_CONFIG
                if (s_pool_state.lighting[zone_idx].configured) {
                    mqtt_publish_light(&s_pool_state, s_pool_state.lighting[zone_idx].zone);
                }

                xSemaphoreGive(s_pool_state_mutex);
            }
        } else if (channel >= 0xE0 && channel <= 0xE3) {
            // Lighting zone active state (E0-E3 = zones 1-4)
            uint8_t zone = channel - 0xE0 + 1;
            uint8_t active = value2;
            ESP_LOGI(TAG, "%s Lighting zone %d active - %s", addr_info, zone, active ? "Yes" : "No");

            // Update pool state (only publish if zone is already configured)
            if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint8_t zone_idx = channel - 0xE0;
                s_pool_state.lighting[zone_idx].active = (active != 0);
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Only publish if this zone was configured via MSG_TYPE_LIGHT_CONFIG
                if (s_pool_state.lighting[zone_idx].configured) {
                    mqtt_publish_light(&s_pool_state, s_pool_state.lighting[zone_idx].zone);
                }

                xSemaphoreGive(s_pool_state_mutex);
            }
        } else if (channel >= 0x6C && channel <= 0x73) {
            // Channel type configuration (6C-73 = channels 1-8)
            uint8_t ch_num = channel - 0x6C + 1;
            uint8_t ch_type = value2;
            if (ch_type == CHANNEL_END) {
                ESP_LOGI(TAG, "%s Channel %d type - Unused (last channel)", addr_info, ch_num);
            } else if (ch_type == CHANNEL_UNUSED) {
                ESP_LOGI(TAG, "%s Channel %d type - Unused", addr_info, ch_num);
            } else {
                const char *type_name = (ch_type < CHANNEL_TYPE_COUNT) ? CHANNEL_TYPE_NAMES[ch_type] : "Unknown";
                ESP_LOGI(TAG, "%s Channel %d type - %s (%d)", addr_info, ch_num, type_name, ch_type);

                // Update pool state
                if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    s_pool_state.channels[ch_num - 1].type = ch_type;
                    s_pool_state.channels[ch_num - 1].configured = true;
                    s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    xSemaphoreGive(s_pool_state_mutex);
                }
            }

        } else {
            ESP_LOGI(TAG, "%s Type 0x38 - Channel=0x%02X, value1=%d, value2=%d", addr_info, channel, value1, value2);
        }
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_CONFIG) && memcmp(data, MSG_TYPE_CONFIG, sizeof(MSG_TYPE_CONFIG)) == 0) {
        uint8_t config_byte = data[10];
        const char *scale_str = (config_byte & 0x10) ? "Fahrenheit" : "Celsius";
        ESP_LOGI(TAG, "%s Config - temperature scale=%s", addr_info, scale_str);

        // Update pool state
        if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_pool_state.temp_scale_fahrenheit = (config_byte & 0x10) != 0;
            s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(s_pool_state_mutex);
        }
        return false;
    }
    else if (len >= sizeof(MSG_TYPE_MODE) && memcmp(data, MSG_TYPE_MODE, sizeof(MSG_TYPE_MODE)) == 0) {
        uint8_t mode = data[10];
        const char *mode_str = (mode == 0x00) ? "Spa" : (mode == 0x01) ? "Pool" : "Unknown";
        ESP_LOGI(TAG, "%s Mode - %s", addr_info, mode_str);

        // Update pool state
        if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_pool_state.mode = mode;
            s_pool_state.mode_valid = true;
            s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Publish to MQTT
            mqtt_publish_mode(&s_pool_state);

            xSemaphoreGive(s_pool_state_mutex);
        }
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_CHANNELS) + 2 && memcmp(data, MSG_TYPE_CHANNELS, sizeof(MSG_TYPE_CHANNELS)) == 0) {
        uint8_t bitmask = data[10];
        ESP_LOGI(TAG, "%s Active channels - 0x%02X [%c%c%c%c%c%c%c%c]",
                 addr_info, bitmask,
                 (bitmask & 0x80) ? '8' : '-',
                 (bitmask & 0x40) ? '7' : '-',
                 (bitmask & 0x20) ? '6' : '-',
                 (bitmask & 0x10) ? '5' : '-',
                 (bitmask & 0x08) ? '4' : '-',
                 (bitmask & 0x04) ? '3' : '-',
                 (bitmask & 0x02) ? '2' : '-',
                 (bitmask & 0x01) ? '1' : '-');
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_CHANNEL_STATUS) + 1 && memcmp(data, MSG_TYPE_CHANNEL_STATUS, sizeof(MSG_TYPE_CHANNEL_STATUS)) == 0) {
        uint8_t num_channels = data[10];
        ESP_LOGI(TAG, "%s Channel status (%d channels):", addr_info, num_channels);
        int idx = 11;  // Channel data starts after header
        int ch_num = 1;
        bool past_end = false;

        // Update pool state
        if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_pool_state.num_channels = num_channels;

            while (ch_num <= num_channels) {
                if (past_end || idx + 2 >= len - 1) {
                    ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                    ch_num++;
                    continue;
                }
                uint8_t ch_type = data[idx];
                if (ch_type == CHANNEL_END) {
                    past_end = true;
                    ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                    ch_num++;
                    continue;
                }
                uint8_t state = data[idx + 1];
                const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";

                if (ch_type == CHANNEL_UNUSED) {
                    ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                    s_pool_state.channels[ch_num - 1].configured = false;
                } else {
                    const char *type_name = (ch_type < CHANNEL_TYPE_COUNT) ? CHANNEL_TYPE_NAMES[ch_type] : "Unknown";
                    ESP_LOGI(TAG, "  Ch%d: %s (%d) = %s", ch_num, type_name, ch_type, state_name);

                    // Update channel state
                    s_pool_state.channels[ch_num - 1].id = ch_num;
                    s_pool_state.channels[ch_num - 1].type = ch_type;
                    s_pool_state.channels[ch_num - 1].state = state;
                    s_pool_state.channels[ch_num - 1].configured = true;

                    // Publish to MQTT (outside mutex to avoid blocking)
                    mqtt_publish_channel(&s_pool_state, ch_num);
                }
                idx += 3;
                ch_num++;
            }
            s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(s_pool_state_mutex);
        }
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_TEMP_SETTING) && memcmp(data, MSG_TYPE_TEMP_SETTING, sizeof(MSG_TYPE_TEMP_SETTING)) == 0) {
        uint8_t spa_set_temp = data[10];
        uint8_t pool_set_temp = data[11];
        ESP_LOGI(TAG, "%s Temperature settings - spa_set_temp=%d, pool_set_temp=%d", addr_info, spa_set_temp, pool_set_temp);

        // Update pool state
        if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_pool_state.spa_setpoint = spa_set_temp;
            s_pool_state.pool_setpoint = pool_set_temp;
            s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Publish to MQTT
            mqtt_publish_temperature(&s_pool_state);

            xSemaphoreGive(s_pool_state_mutex);
        }
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_TEMP_READING) && memcmp(data, MSG_TYPE_TEMP_READING, sizeof(MSG_TYPE_TEMP_READING)) == 0) {
        uint8_t current_temp = data[10];
        ESP_LOGI(TAG, "%s Current temperature - %d", addr_info, current_temp);

        // Update pool state
        if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_pool_state.current_temp = current_temp;
            s_pool_state.temp_valid = true;
            s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Publish to MQTT
            mqtt_publish_temperature(&s_pool_state);

            xSemaphoreGive(s_pool_state_mutex);
        }
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_HEATER) && memcmp(data, MSG_TYPE_HEATER, sizeof(MSG_TYPE_HEATER)) == 0) {
        uint8_t heater_state = data[11];
        ESP_LOGI(TAG, "%s Heater - %s", addr_info, heater_state ? "On" : "Off");

        // Update pool state
        if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_pool_state.heater_on = (heater_state != 0);
            s_pool_state.heater_valid = true;
            s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Publish to MQTT
            mqtt_publish_heater(&s_pool_state);

            xSemaphoreGive(s_pool_state_mutex);
        }
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_CHLOR) + 6 && memcmp(data, MSG_TYPE_CHLOR, sizeof(MSG_TYPE_CHLOR)) == 0) {
        const uint8_t *sub = &data[7];
        uint16_t value = data[11] | (data[12] << 8);  // little endian

        
        if (memcmp(sub, CHLOR_PH_SETPOINT, sizeof(CHLOR_PH_SETPOINT)) == 0) {
            ESP_LOGI(TAG, "%s Chlorinator pH setpoint - %.1f", addr_info, value / 10.0);

            // Update pool state
            if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_pool_state.ph_setpoint = value;
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Publish to MQTT
                mqtt_publish_chlorinator(&s_pool_state);

                xSemaphoreGive(s_pool_state_mutex);
            }
            return true;
        }
        else if (memcmp(sub, CHLOR_ORP_SETPOINT, sizeof(CHLOR_ORP_SETPOINT)) == 0) {
            ESP_LOGI(TAG, "%s Chlorinator ORP setpoint - %d mV", addr_info, value);

            // Update pool state
            if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_pool_state.orp_setpoint = value;
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Publish to MQTT
                mqtt_publish_chlorinator(&s_pool_state);

                xSemaphoreGive(s_pool_state_mutex);
            }
            return true;
        }
        else if (memcmp(sub, CHLOR_PH_READING, sizeof(CHLOR_PH_READING)) == 0) {
            ESP_LOGI(TAG, "%s Chlorinator pH reading - %.1f", addr_info, value / 10.0);

            // Update pool state
            if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_pool_state.ph_reading = value;
                s_pool_state.ph_valid = true;
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Publish to MQTT
                mqtt_publish_chlorinator(&s_pool_state);

                xSemaphoreGive(s_pool_state_mutex);
            }
            return true;
        }
        else if (memcmp(sub, CHLOR_ORP_READING, sizeof(CHLOR_ORP_READING)) == 0) {
            ESP_LOGI(TAG, "%s Chlorinator ORP reading - %d mV", addr_info, value);

            // Update pool state
            if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_pool_state.orp_reading = value;
                s_pool_state.orp_valid = true;
                s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Publish to MQTT
                mqtt_publish_chlorinator(&s_pool_state);

                xSemaphoreGive(s_pool_state_mutex);
            }
            return true;
        }
    }
    else if (len >= sizeof(MSG_TYPE_SERIAL_NUMBER) + 6 && memcmp(data, MSG_TYPE_SERIAL_NUMBER, sizeof(MSG_TYPE_SERIAL_NUMBER)) == 0) {
        // Serial number is in bytes 11-14 (little endian)
        uint32_t serial = data[11] | (data[12] << 8) | (data[13] << 16) | (data[14] << 24);
        ESP_LOGI(TAG, "%s Serial number - %u (0x%08X)", addr_info, serial, serial);

        // Update pool state
        if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_pool_state.serial_number = serial;
            s_pool_state.serial_number_valid = true;
            s_pool_state.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(s_pool_state_mutex);
        }
        return true;
    }

    return false;
}


// ======================================================
// Message verification and diagnostics
// ======================================================

/**
 * Verify checksum for Astral protocol messages.
 * Checksum = sum of all data bytes from index 10 to (len-3), low byte only.
 * The checksum byte is at (len-2).
 *
 * Returns true if checksum is valid, false otherwise.
 */
bool verify_message_checksum(const uint8_t *data, int len)
{
    // Must have at least: 02 [10 bytes] [data] [checksum] 03
    if (len < 13 || data[0] != 0x02 || data[len - 1] != 0x03) {
        return false;
    }

    // Calculate checksum: sum bytes from index 10 to (len-3) inclusive
    uint32_t sum = 0;
    for (int i = 10; i < len - 2; i++) {
        sum += data[i];
    }

    uint8_t calculated_checksum = sum & 0xFF;
    uint8_t received_checksum = data[len - 2];

    return (calculated_checksum == received_checksum);
}

// Track last sent message for loopback verification
static uint8_t s_last_tx_msg[256];
static int s_last_tx_len = 0;
static TickType_t s_last_tx_time = 0;

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
        pos += snprintf(&hex_log[pos], sizeof(hex_log) - pos, "%02X ", msg_buf[i]);
    }
    hex_log[pos] = '\0';

    ESP_LOGI(TAG, "TX: %s(%d bytes)", hex_log, msg_len);

    // Store for loopback verification
    memcpy(s_last_tx_msg, msg_buf, msg_len);
    s_last_tx_len = msg_len;
    s_last_tx_time = xTaskGetTickCount();

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
    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = 80;

    esp_err_t err = httpd_start(&s_httpd_handle, &httpd_config);
    if (err == ESP_OK) {
        // Register URI handlers
        web_handlers_register(s_httpd_handle);
        ESP_LOGI(TAG, "HTTP server started - Navigate to http://192.168.4.1");
        ESP_LOGI(TAG, "Pool status available at http://192.168.4.1/status");
        ESP_LOGI(TAG, "MQTT configuration at http://192.168.4.1/mqtt_config");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
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
// TCP server task: bridge UART <-> TCP client
// ======================================================

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int listen_sock = -1;
    int client_sock = -1;
    uint8_t uart_buf[256];
    uint8_t tcp_buf[256];
    char line_buf[512];  // Buffer for accumulating incoming lines
    int line_pos = 0;

    // Create listening socket
    while (listen_sock < 0) {
        struct sockaddr_in listen_addr = {0};
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_addr.sin_port = htons(TCP_PORT);

        listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(listen_sock, (struct sockaddr *)&listen_addr,
                 sizeof(listen_addr)) < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (listen(listen_sock, 1) < 0) {
            ESP_LOGE(TAG, "Error during listen: errno %d", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Make listening socket non-blocking
        int flags = fcntl(listen_sock, F_GETFL, 0);
        fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK);

        ESP_LOGI(TAG, "TCP server listening on port %d", TCP_PORT);
    }

    // Main loop - always reads UART, optionally bridges to TCP client
    while (1) {
        // Check for new client connection (non-blocking)
        if (client_sock < 0) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            client_sock = accept(listen_sock,
                                 (struct sockaddr *)&client_addr,
                                 &addr_len);
            if (client_sock >= 0) {
                inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
                ESP_LOGI(TAG, "Client connected from %s", addr_str);

                const char *hello =
                    "Connected to ESP32-C6 pool bus bridge.\r\n"
                    "UART bytes will be shown here in hex.\r\n"
                    "Send hex strings (e.g., '02 00 50 FF FF 03') to transmit to the bus.\r\n\r\n";
                send(client_sock, hello, strlen(hello), 0);

                // Reset line buffer for new client
                line_pos = 0;
            }
        }

        // 1. UART RX - always read and log
        int len = uart_read_bytes(BUS_UART_NUM,
                                  uart_buf,
                                  sizeof(uart_buf),
                                  10 / portTICK_PERIOD_MS); // 10 ms
        if (len > 0) {
            led_flash_rx();

            // Format as hex string
            char hexLine[3 * sizeof(uart_buf) + 4];
            int pos = 0;
            for (int i = 0; i < len; ++i) {
                if (pos < (int)(sizeof(hexLine) - 4)) {
                    pos += snprintf(&hexLine[pos],
                                    sizeof(hexLine) - pos,
                                    "%02X ",
                                    uart_buf[i]);
                }
            }
            hexLine[pos] = '\0';

            // Check if this is our own transmitted message (loopback verification)
            bool is_loopback = false;
            if (s_last_tx_len > 0 && len == s_last_tx_len) {
                TickType_t time_since_tx = xTaskGetTickCount() - s_last_tx_time;
                if (time_since_tx < pdMS_TO_TICKS(500)) {  // Within 500ms of TX
                    if (memcmp(uart_buf, s_last_tx_msg, len) == 0) {
                        is_loopback = true;
                        ESP_LOGI(TAG, "RX LOOPBACK (our TX echoed): %s", hexLine);
                        s_last_tx_len = 0;  // Clear so we don't match again
                    }
                }
            }

            // Decode the message, log hex only if not decoded and not loopback
            if (!is_loopback && !decode_message(uart_buf, len)) {
                ESP_LOGI(TAG, "RX: %s", hexLine);
            }

            // Send to client if connected
            if (client_sock >= 0) {
                hexLine[pos++] = '\r';
                hexLine[pos++] = '\n';
                int sent = send(client_sock, hexLine, pos, 0);
                if (sent < 0) {
                    ESP_LOGW(TAG, "Client send error: errno %d", errno);
                    shutdown(client_sock, SHUT_RDWR);
                    close(client_sock);
                    client_sock = -1;
                }
            }
        }

        // 2. TCP -> UART (only if client connected)
        if (client_sock >= 0) {
            int r = recv(client_sock,
                         tcp_buf,
                         sizeof(tcp_buf),
                         MSG_DONTWAIT);
            if (r > 0) {
                // Process received characters
                for (int i = 0; i < r; i++) {
                    char c = tcp_buf[i];

                    // Echo the character back to the client
                    send(client_sock, &c, 1, 0);

                    if (c == '\n' || c == '\r') {
                        // End of line - process the accumulated command
                        if (line_pos > 0) {
                            line_buf[line_pos] = '\0';

                            // Parse and send the hex string
                            int sent = bus_send_message(line_buf);
                            if (sent > 0) {
                                const char *ok_msg = "OK - sent\r\n";
                                send(client_sock, ok_msg, strlen(ok_msg), 0);
                            } else {
                                const char *err_msg = "ERROR - invalid hex string\r\n";
                                send(client_sock, err_msg, strlen(err_msg), 0);
                            }

                            line_pos = 0;  // Reset for next line
                        }
                    } else if (c == 0x08 || c == 0x7F) {
                        // Backspace or delete - remove last character
                        if (line_pos > 0) {
                            line_pos--;
                        }
                    } else {
                        // Add character to line buffer
                        if (line_pos < (int)sizeof(line_buf) - 1) {
                            line_buf[line_pos++] = c;
                        } else {
                            // Buffer full - reset
                            const char *overflow_msg = "\r\nERROR - line too long\r\n";
                            send(client_sock, overflow_msg, strlen(overflow_msg), 0);
                            line_pos = 0;
                        }
                    }
                }
            } else if (r == 0) {
                ESP_LOGI(TAG, "Client disconnected");
                shutdown(client_sock, SHUT_RDWR);
                close(client_sock);
                client_sock = -1;
                line_pos = 0;  // Reset line buffer
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "Client recv error: errno %d", errno);
                    shutdown(client_sock, SHUT_RDWR);
                    close(client_sock);
                    client_sock = -1;
                    line_pos = 0;  // Reset line buffer
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
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

    // Initialize pool state mutex
    s_pool_state_mutex = xSemaphoreCreateMutex();
    if (s_pool_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create pool state mutex");
        return;
    }

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
        httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
        httpd_config.server_port = 80;

        esp_err_t err = httpd_start(&s_httpd_handle, &httpd_config);
        if (err == ESP_OK) {
            // Register URI handlers
            web_handlers_register(s_httpd_handle);
            ESP_LOGI(TAG, "HTTP server started - Pool status available at http://%s/status", s_device_ip_address);
            ESP_LOGI(TAG, "MQTT configuration at http://%s/mqtt_config", s_device_ip_address);
        } else {
            ESP_LOGW(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        }

        // Start TCP server for UART bridge
        xTaskCreate(tcp_server_task,
                    "tcp_server_task",
                    8192,  // Increased from 4096 to handle MQTT discovery publishing
                    NULL,
                    5,
                    NULL);
    }
}