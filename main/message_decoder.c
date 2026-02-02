#include "message_decoder.h"
#include "config.h"
#include "mqtt_publish.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "MSG_DECODER";

// ======================================================
// Message type patterns
// ======================================================

// Message type patterns (messages start with 0x02, end with 0x03)
// 50 Main Controller (Connect 10)
static const uint8_t MSG_TYPE_REGISTER_STATUS[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x0F, 0x17};
static const uint8_t MSG_TYPE_REGISTER_LABEL[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x1A, 0x22};
static const uint8_t MSG_TYPE_LIGHTING_VALVE_LABEL[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x16, 0x1E};
static const uint8_t MSG_TYPE_38_BASE[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38};  // Shorter pattern for channel names
static const uint8_t MSG_TYPE_TEMP_SETTING[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x17, 0x10};
static const uint8_t MSG_TYPE_CONFIG[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x26, 0x0E};
static const uint8_t MSG_TYPE_MODE[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x14, 0x0D, 0xF1};
static const uint8_t MSG_TYPE_CHANNELS[] = {0x02, 0x00, 0x50, 0x00, 0x6F, 0x80, 0x00, 0x0D, 0x0D, 0x5B};
static const uint8_t MSG_TYPE_CHANNEL_STATUS[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x0B, 0x25, 0x00};
static const uint8_t MSG_TYPE_LIGHT_CONFIG[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x06, 0x0E, 0xE4};
static const uint8_t MSG_TYPE_CONTROLLER_TIME[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0xFD, 0x0F, 0xDC};
static const uint8_t MSG_TYPE_TOUCHSCREEN_VERSION[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x0A, 0x0E, 0xE8};

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

// F0 Internet Gateway
static const uint8_t MSG_TYPE_SERIAL_NUMBER[] = {0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00, 0x37, 0x11};
static const uint8_t MSG_TYPE_GATEWAY_IP[] = {0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00, 0x37, 0x15};
static const uint8_t MSG_TYPE_GATEWAY_COMMS[] = {0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00, 0x37, 0x0F};
static const uint8_t MSG_TYPE_REGISTER_READ_REQUEST[] = {0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00, 0x39, 0x0E, 0xB7};

// ======================================================
// Lookup tables and constants
// ======================================================

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

// Channel state names
const char *CHANNEL_STATE_NAMES[] = {
    "Off",          // 0
    "Auto",         // 1
    "On",           // 2
    "Low Speed",    // 3
    "Medium Speed", // 4
    "High Speed",   // 5
};

// Lighting state names
const char *LIGHTING_STATE_NAMES[] = {
    "Off",          // 0
    "Auto",         // 1
    "On",           // 2
};

// Day of week names
const char *DAY_OF_WEEK_NAMES[] = {
    "Monday",       // 0
    "Tuesday",      // 1
    "Wednesday",    // 2
    "Thursday",     // 3
    "Friday",       // 4
    "Saturday",     // 5
    "Sunday",       // 6
};
#define DAY_OF_WEEK_COUNT 7

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

// Gateway comms status lookup table
typedef struct {
    uint16_t code;
    const char *text;
} gateway_comms_status_t;

static const gateway_comms_status_t GATEWAY_COMMS_STATUS[] = {
    {0, "Idle"},
    {256, "No suitable interfaces ready"},
    {513, "DNS resolve error"},
    {769, "Internal error creating local socket"},
    {1024, "Connecting to server"},
    {1025, "Failed to connect"},
    {32768, "Connection open"},
    {32769, "Communicating with server"},
    {61440, "Connection closed"},
    {61441, "Communication error with server"},
    {61442, "Communication error with server"},
    {61443, "Communication error with server"},
    {61444, "Communication error with server"},
    // Add more status codes here as they are discovered
};
#define GATEWAY_COMMS_STATUS_COUNT (sizeof(GATEWAY_COMMS_STATUS) / sizeof(GATEWAY_COMMS_STATUS[0]))

// ======================================================
// Helper functions
// ======================================================

const char* get_device_name(uint8_t addr_hi, uint8_t addr_lo)
{
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

const char* get_gateway_comms_status_text(uint16_t code)
{
    for (int i = 0; i < GATEWAY_COMMS_STATUS_COUNT; i++) {
        if (GATEWAY_COMMS_STATUS[i].code == code) {
            return GATEWAY_COMMS_STATUS[i].text;
        }
    }
    return "Unknown";
}

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

// ======================================================
// Main decoder function
// ======================================================

bool decode_message(const uint8_t *data, int len, message_decoder_context_t *ctx)
{
    if (!ctx || !ctx->pool_state) {
        return false;
    }

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

    // Extract data payload section (bytes 10 to len-3)
    // Message format: [START=0][SRC=1-2][DST=3-4][CTRL=5-6][CMD=7-9][DATA=10...][CHECKSUM=len-2][END=len-1]
    const uint8_t *payload = &data[10];
    int payload_len = (len >= 13) ? (len - 12) : 0;  // len - 12 = len - (10 header + 2 trailer)

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

    // Register label messages (payload[0] = register ID, payload[2+] = label string)
    if (len >= sizeof(MSG_TYPE_REGISTER_LABEL) + 3 && memcmp(data, MSG_TYPE_REGISTER_LABEL, sizeof(MSG_TYPE_REGISTER_LABEL)) == 0) {
        if (payload_len < 3) return false;
        uint8_t reg_id = payload[0];
        const char *label = (const char *)&payload[2];

        ESP_LOGI(TAG, "%s Register 0x%02X label - \"%s\"", addr_info, reg_id, label);

        // Update pool state - find or create entry for this register
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            // Find existing entry or first available slot
            int slot = -1;
            for (int i = 0; i < 32; i++) {
                if (ctx->pool_state->register_labels[i].valid && ctx->pool_state->register_labels[i].reg_id == reg_id) {
                    slot = i;  // Found existing entry
                    break;
                } else if (!ctx->pool_state->register_labels[i].valid && slot == -1) {
                    slot = i;  // Remember first available slot
                }
            }

            if (slot >= 0) {
                ctx->pool_state->register_labels[slot].reg_id = reg_id;
                strncpy(ctx->pool_state->register_labels[slot].label, label, sizeof(ctx->pool_state->register_labels[slot].label) - 1);
                ctx->pool_state->register_labels[slot].label[sizeof(ctx->pool_state->register_labels[slot].label) - 1] = '\0';
                ctx->pool_state->register_labels[slot].valid = true;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            }

            xSemaphoreGive(ctx->state_mutex);
        }
        return true;
    }

    // Lighting/valve label messages (payload[0] = register ID 0xD0-0xD3, payload[2+] = label string)
    if (len >= sizeof(MSG_TYPE_LIGHTING_VALVE_LABEL) + 3 && memcmp(data, MSG_TYPE_LIGHTING_VALVE_LABEL, sizeof(MSG_TYPE_LIGHTING_VALVE_LABEL)) == 0) {
        if (payload_len < 3) return false;
        uint8_t reg_id = payload[0];
        const char *label = (const char *)&payload[2];

        // Identify which zone this is (0xD0-0xD3 = zones 1-4)
        if (reg_id >= 0xD0 && reg_id <= 0xD3) {
            uint8_t zone_num = reg_id - 0xD0 + 1;
            ESP_LOGI(TAG, "%s Lighting/Valve zone %d label (0x%02X) - \"%s\"", addr_info, zone_num, reg_id, label);
        } else {
            ESP_LOGI(TAG, "%s Register 0x%02X label - \"%s\"", addr_info, reg_id, label);
        }

        // Update pool state - find or create entry for this register
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            // Find existing entry or first available slot
            int slot = -1;
            for (int i = 0; i < 32; i++) {
                if (ctx->pool_state->register_labels[i].valid && ctx->pool_state->register_labels[i].reg_id == reg_id) {
                    slot = i;  // Found existing entry
                    break;
                } else if (!ctx->pool_state->register_labels[i].valid && slot == -1) {
                    slot = i;  // Remember first available slot
                }
            }

            if (slot >= 0) {
                ctx->pool_state->register_labels[slot].reg_id = reg_id;
                strncpy(ctx->pool_state->register_labels[slot].label, label, sizeof(ctx->pool_state->register_labels[slot].label) - 1);
                ctx->pool_state->register_labels[slot].label[sizeof(ctx->pool_state->register_labels[slot].label) - 1] = '\0';
                ctx->pool_state->register_labels[slot].valid = true;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            }

            xSemaphoreGive(ctx->state_mutex);
        }
        return true;
    }

    // Channel name messages (payload[0] = 0x7C-0x83 for channels 1-8)
    if (len >= sizeof(MSG_TYPE_38_BASE) + 5 && memcmp(data, MSG_TYPE_38_BASE, sizeof(MSG_TYPE_38_BASE)) == 0) {
        if (payload_len < 5) return false;
        uint8_t ch_id = payload[0];
        if (ch_id >= 0x7C && ch_id <= 0x83) {
            uint8_t ch_num = ch_id - 0x7C + 1;
            const char *name = (const char *)&payload[2];

            // Check if it's an empty/unused channel (first byte is 0x00)
            if (payload[2] == 0x00) {
                ESP_LOGI(TAG, "%s Channel %d name - (empty)", addr_info, ch_num);
            } else {
                ESP_LOGI(TAG, "%s Channel %d name - \"%s\"", addr_info, ch_num, name);

                // Update pool state
                if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    if (ch_num <= 8) {
                        strncpy(ctx->pool_state->channels[ch_num - 1].name, name, sizeof(ctx->pool_state->channels[ch_num - 1].name) - 1);
                        ctx->pool_state->channels[ch_num - 1].id = ch_num;
                        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    }
                    xSemaphoreGive(ctx->state_mutex);
                }
            }
            return true;
        }
    }

    // Lighting zone configuration messages - tells us which zones are actually installed
    if (len >= sizeof(MSG_TYPE_LIGHT_CONFIG) + 4 && memcmp(data, MSG_TYPE_LIGHT_CONFIG, sizeof(MSG_TYPE_LIGHT_CONFIG)) == 0) {
        if (payload_len < 1) return false;
        uint8_t zone_idx = payload[0];
        if (zone_idx <= 3) {
            // Mark this zone as configured (only publish if this is the first time)
            bool should_publish = false;
            pool_state_t state_snapshot;
            if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                bool was_configured = ctx->pool_state->lighting[zone_idx].configured;

                ctx->pool_state->lighting[zone_idx].zone = zone_idx + 1;
                ctx->pool_state->lighting[zone_idx].configured = true;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Only log and publish if this is the first time we're marking it as configured
                if (!was_configured) {
                    ESP_LOGI(TAG, "%s Lighting zone %d configured", addr_info, zone_idx + 1);
                    should_publish = true;
                }

                // Take snapshot while holding mutex
                state_snapshot = *ctx->pool_state;
                xSemaphoreGive(ctx->state_mutex);
            }

            // Publish MQTT discovery for this zone (outside mutex)
            if (should_publish && ctx->enable_mqtt) {
                mqtt_publish_light(&state_snapshot, zone_idx + 1);
            }
        }
        return true;
    }

    // Register status messages (lighting, channel types, etc.)
    if (len >= sizeof(MSG_TYPE_REGISTER_STATUS) && memcmp(data, MSG_TYPE_REGISTER_STATUS, sizeof(MSG_TYPE_REGISTER_STATUS)) == 0) {
        if (payload_len < 3) return false;
        uint8_t channel = payload[0];
        uint8_t value1 = payload[1];
        uint8_t value2 = payload[2];

        if (channel >= 0xC0 && channel <= 0xC3) {
            // Lighting zone state
            const char *state = (value2 < LIGHTING_STATE_COUNT) ? LIGHTING_STATE_NAMES[value2] : "Unknown";
            ESP_LOGI(TAG, "%s Lighting zone %d - %s", addr_info, channel - 0xC0 + 1, state);

            // Update pool state and create snapshot (only publish if zone is already configured)
            bool should_publish = false;
            uint8_t zone_num = 0;
            pool_state_t state_snapshot;
            if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                uint8_t zone_idx = channel - 0xC0;
                ctx->pool_state->lighting[zone_idx].zone = zone_idx + 1;
                ctx->pool_state->lighting[zone_idx].state = value2;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Only publish if this zone was configured via MSG_TYPE_LIGHT_CONFIG
                if (ctx->pool_state->lighting[zone_idx].configured) {
                    should_publish = true;
                    zone_num = ctx->pool_state->lighting[zone_idx].zone;
                }

                // Take snapshot while holding mutex
                state_snapshot = *ctx->pool_state;
                xSemaphoreGive(ctx->state_mutex);
            }

            // Publish using snapshot (outside mutex)
            if (should_publish && ctx->enable_mqtt) {
                mqtt_publish_light(&state_snapshot, zone_num);
            }
        } else if (channel >= 0xD0 && channel <= 0xD3) {
            // Lighting zone color (D0-D3 = zones 1-4)
            uint8_t zone = channel - 0xD0 + 1;
            uint8_t color = value2;
            const char *color_name = (color < LIGHTING_COLOR_COUNT) ? LIGHTING_COLOR_NAMES[color] : "Unknown";
            ESP_LOGI(TAG, "%s Lighting zone %d color - %s (%d)", addr_info, zone, color_name, color);

            // Update pool state and create snapshot (only publish if zone is already configured)
            bool should_publish = false;
            uint8_t zone_num = 0;
            pool_state_t state_snapshot;
            if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                uint8_t zone_idx = channel - 0xD0;
                ctx->pool_state->lighting[zone_idx].color = color;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Only publish if this zone was configured via MSG_TYPE_LIGHT_CONFIG
                if (ctx->pool_state->lighting[zone_idx].configured) {
                    should_publish = true;
                    zone_num = ctx->pool_state->lighting[zone_idx].zone;
                }

                // Take snapshot while holding mutex
                state_snapshot = *ctx->pool_state;
                xSemaphoreGive(ctx->state_mutex);
            }

            // Publish using snapshot (outside mutex)
            if (should_publish && ctx->enable_mqtt) {
                mqtt_publish_light(&state_snapshot, zone_num);
            }
        } else if (channel >= 0xE0 && channel <= 0xE3) {
            // Lighting zone active state (E0-E3 = zones 1-4)
            uint8_t zone = channel - 0xE0 + 1;
            uint8_t active = value2;
            ESP_LOGI(TAG, "%s Lighting zone %d active - %s", addr_info, zone, active ? "Yes" : "No");

            // Update pool state and create snapshot (only publish if zone is already configured)
            bool should_publish = false;
            uint8_t zone_num = 0;
            pool_state_t state_snapshot;
            if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                uint8_t zone_idx = channel - 0xE0;
                ctx->pool_state->lighting[zone_idx].active = (active != 0);
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Only publish if this zone was configured via MSG_TYPE_LIGHT_CONFIG
                if (ctx->pool_state->lighting[zone_idx].configured) {
                    should_publish = true;
                    zone_num = ctx->pool_state->lighting[zone_idx].zone;
                }

                // Take snapshot while holding mutex
                state_snapshot = *ctx->pool_state;
                xSemaphoreGive(ctx->state_mutex);
            }

            // Publish using snapshot (outside mutex)
            if (should_publish && ctx->enable_mqtt) {
                mqtt_publish_light(&state_snapshot, zone_num);
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
                if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                    ctx->pool_state->channels[ch_num - 1].type = ch_type;
                    ctx->pool_state->channels[ch_num - 1].configured = true;
                    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    xSemaphoreGive(ctx->state_mutex);
                }
            }
        } else {
            // Unknown register - likely a register read response
            ESP_LOGI(TAG, "%s Register read response - Register=0x%02X, value1=0x%02X, value2=0x%02X", addr_info, channel, value1, value2);
        }
        return true;
    }

    // Configuration messages
    if (len >= sizeof(MSG_TYPE_CONFIG) && memcmp(data, MSG_TYPE_CONFIG, sizeof(MSG_TYPE_CONFIG)) == 0) {
        if (payload_len < 1) return false;
        uint8_t config_byte = payload[0];
        const char *scale_str = (config_byte & 0x10) ? "Fahrenheit" : "Celsius";
        ESP_LOGI(TAG, "%s Config - temperature scale=%s", addr_info, scale_str);

        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->temp_scale_fahrenheit = (config_byte & 0x10) != 0;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
        return false;
    }

    // Mode messages (Spa/Pool)
    if (len >= sizeof(MSG_TYPE_MODE) && memcmp(data, MSG_TYPE_MODE, sizeof(MSG_TYPE_MODE)) == 0) {
        if (payload_len < 1) return false;
        uint8_t mode = payload[0];
        const char *mode_str = (mode == 0x00) ? "Spa" : (mode == 0x01) ? "Pool" : "Unknown";
        ESP_LOGI(TAG, "%s Mode - %s", addr_info, mode_str);

        // Update pool state and create snapshot for publishing
        pool_state_t state_snapshot;
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->mode = mode;
            ctx->pool_state->mode_valid = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Take snapshot while holding mutex
            state_snapshot = *ctx->pool_state;
            xSemaphoreGive(ctx->state_mutex);
        }

        // Publish to MQTT using snapshot (outside mutex to avoid blocking)
        if (ctx->enable_mqtt) {
            mqtt_publish_mode(&state_snapshot);
        }

        return true;
    }

    // Active channels bitmask
    if (len >= sizeof(MSG_TYPE_CHANNELS) + 2 && memcmp(data, MSG_TYPE_CHANNELS, sizeof(MSG_TYPE_CHANNELS)) == 0) {
        if (payload_len < 1) return false;
        uint8_t bitmask = payload[0];
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

    // Channel status messages
    if (len >= sizeof(MSG_TYPE_CHANNEL_STATUS) + 1 && memcmp(data, MSG_TYPE_CHANNEL_STATUS, sizeof(MSG_TYPE_CHANNEL_STATUS)) == 0) {
        if (payload_len < 1) return false;
        uint8_t num_channels = payload[0];
        ESP_LOGI(TAG, "%s Channel status (%d channels):", addr_info, num_channels);
        int payload_idx = 1;  // Channel data starts at payload[1]
        int ch_num = 1;
        bool past_end = false;
        uint8_t channels_to_publish[8] = {0};  // Track which channels to publish
        int num_to_publish = 0;

        // Update pool state
        pool_state_t state_snapshot;
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->num_channels = num_channels;

            while (ch_num <= num_channels) {
                if (past_end || payload_idx + 2 >= payload_len) {
                    ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                    ch_num++;
                    continue;
                }
                uint8_t ch_type = payload[payload_idx];
                if (ch_type == CHANNEL_END) {
                    past_end = true;
                    ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                    ch_num++;
                    continue;
                }
                uint8_t state = payload[payload_idx + 1];
                const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";

                if (ch_type == CHANNEL_UNUSED) {
                    ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                    ctx->pool_state->channels[ch_num - 1].configured = false;
                } else {
                    const char *type_name = (ch_type < CHANNEL_TYPE_COUNT) ? CHANNEL_TYPE_NAMES[ch_type] : "Unknown";
                    ESP_LOGI(TAG, "  Ch%d: %s (%d) = %s", ch_num, type_name, ch_type, state_name);

                    // Update channel state
                    ctx->pool_state->channels[ch_num - 1].id = ch_num;
                    ctx->pool_state->channels[ch_num - 1].type = ch_type;
                    ctx->pool_state->channels[ch_num - 1].state = state;
                    ctx->pool_state->channels[ch_num - 1].configured = true;

                    // Mark this channel for publishing
                    channels_to_publish[num_to_publish++] = ch_num;
                }
                payload_idx += 3;
                ch_num++;
            }
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Take snapshot while holding mutex
            state_snapshot = *ctx->pool_state;
            xSemaphoreGive(ctx->state_mutex);
        }

        // Publish all channels using snapshot (outside mutex to avoid blocking)
        if (ctx->enable_mqtt) {
            for (int i = 0; i < num_to_publish; i++) {
                mqtt_publish_channel(&state_snapshot, channels_to_publish[i]);
            }
        }

        return true;
    }

    // Temperature setting messages
    if (len >= sizeof(MSG_TYPE_TEMP_SETTING) && memcmp(data, MSG_TYPE_TEMP_SETTING, sizeof(MSG_TYPE_TEMP_SETTING)) == 0) {
        if (payload_len < 4) return false;
        uint8_t spa_set_temp_c = payload[0];
        uint8_t pool_set_temp_c = payload[1];
        uint8_t spa_set_temp_f = payload[2];
        uint8_t pool_set_temp_f = payload[3];
        ESP_LOGI(TAG, "%s Temperature settings - spa=%d°C/%d°F, pool=%d°C/%d°F",
                 addr_info, spa_set_temp_c, spa_set_temp_f, pool_set_temp_c, pool_set_temp_f);

        // Update pool state and create snapshot for publishing
        pool_state_t state_snapshot;
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->spa_setpoint = spa_set_temp_c;
            ctx->pool_state->pool_setpoint = pool_set_temp_c;
            ctx->pool_state->spa_setpoint_f = spa_set_temp_f;
            ctx->pool_state->pool_setpoint_f = pool_set_temp_f;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Take snapshot while holding mutex
            state_snapshot = *ctx->pool_state;
            xSemaphoreGive(ctx->state_mutex);
        }

        // Publish to MQTT using snapshot (outside mutex to avoid blocking)
        if (ctx->enable_mqtt) {
            mqtt_publish_temperature(&state_snapshot);
        }

        return true;
    }

    // Temperature reading messages
    if (len >= sizeof(MSG_TYPE_TEMP_READING) && memcmp(data, MSG_TYPE_TEMP_READING, sizeof(MSG_TYPE_TEMP_READING)) == 0) {
        if (payload_len < 1) return false;
        uint8_t current_temp = payload[0];
        ESP_LOGI(TAG, "%s Current temperature - %d", addr_info, current_temp);

        // Update pool state and create snapshot for publishing
        pool_state_t state_snapshot;
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->current_temp = current_temp;
            ctx->pool_state->temp_valid = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Take snapshot while holding mutex
            state_snapshot = *ctx->pool_state;
            xSemaphoreGive(ctx->state_mutex);
        }

        // Publish to MQTT using snapshot (outside mutex to avoid blocking)
        if (ctx->enable_mqtt) {
            mqtt_publish_temperature(&state_snapshot);
        }

        return true;
    }

    // Heater status messages
    if (len >= sizeof(MSG_TYPE_HEATER) && memcmp(data, MSG_TYPE_HEATER, sizeof(MSG_TYPE_HEATER)) == 0) {
        if (payload_len < 2) return false;
        uint8_t heater_state = payload[1];
        ESP_LOGI(TAG, "%s Heater - %s", addr_info, heater_state ? "On" : "Off");

        // Update pool state and create snapshot for publishing
        pool_state_t state_snapshot;
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->heater_on = (heater_state != 0);
            ctx->pool_state->heater_valid = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Take snapshot while holding mutex
            state_snapshot = *ctx->pool_state;
            xSemaphoreGive(ctx->state_mutex);
        }

        // Publish to MQTT using snapshot (outside mutex to avoid blocking)
        if (ctx->enable_mqtt) {
            mqtt_publish_heater(&state_snapshot);
        }

        return true;
    }

    // Chlorinator messages (pH and ORP)
    if (len >= sizeof(MSG_TYPE_CHLOR) + 6 && memcmp(data, MSG_TYPE_CHLOR, sizeof(MSG_TYPE_CHLOR)) == 0) {
        if (payload_len < 3) return false;
        const uint8_t *sub = &data[7];  // Subcommand is in header (bytes 7-9)
        uint16_t value = payload[1] | (payload[2] << 8);  // little endian value at payload[1-2]

        if (memcmp(sub, CHLOR_PH_SETPOINT, sizeof(CHLOR_PH_SETPOINT)) == 0) {
            ESP_LOGI(TAG, "%s Chlorinator pH setpoint - %.1f", addr_info, value / 10.0);

            // Update pool state and create snapshot for publishing
            pool_state_t state_snapshot;
            if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                ctx->pool_state->ph_setpoint = value;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Take snapshot while holding mutex
                state_snapshot = *ctx->pool_state;
                xSemaphoreGive(ctx->state_mutex);
            }

            // Publish to MQTT using snapshot (outside mutex to avoid blocking)
            if (ctx->enable_mqtt) {
                mqtt_publish_chlorinator(&state_snapshot);
            }

            return true;
        }
        else if (memcmp(sub, CHLOR_ORP_SETPOINT, sizeof(CHLOR_ORP_SETPOINT)) == 0) {
            ESP_LOGI(TAG, "%s Chlorinator ORP setpoint - %d mV", addr_info, value);

            // Update pool state and create snapshot for publishing
            pool_state_t state_snapshot;
            if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                ctx->pool_state->orp_setpoint = value;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Take snapshot while holding mutex
                state_snapshot = *ctx->pool_state;
                xSemaphoreGive(ctx->state_mutex);
            }

            // Publish to MQTT using snapshot (outside mutex to avoid blocking)
            if (ctx->enable_mqtt) {
                mqtt_publish_chlorinator(&state_snapshot);
            }

            return true;
        }
        else if (memcmp(sub, CHLOR_PH_READING, sizeof(CHLOR_PH_READING)) == 0) {
            ESP_LOGI(TAG, "%s Chlorinator pH reading - %.1f", addr_info, value / 10.0);

            // Update pool state and create snapshot for publishing
            pool_state_t state_snapshot;
            if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                ctx->pool_state->ph_reading = value;
                ctx->pool_state->ph_valid = true;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Take snapshot while holding mutex
                state_snapshot = *ctx->pool_state;
                xSemaphoreGive(ctx->state_mutex);
            }

            // Publish to MQTT using snapshot (outside mutex to avoid blocking)
            if (ctx->enable_mqtt) {
                mqtt_publish_chlorinator(&state_snapshot);
            }

            return true;
        }
        else if (memcmp(sub, CHLOR_ORP_READING, sizeof(CHLOR_ORP_READING)) == 0) {
            ESP_LOGI(TAG, "%s Chlorinator ORP reading - %d mV", addr_info, value);

            // Update pool state and create snapshot for publishing
            pool_state_t state_snapshot;
            if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
                ctx->pool_state->orp_reading = value;
                ctx->pool_state->orp_valid = true;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

                // Take snapshot while holding mutex
                state_snapshot = *ctx->pool_state;
                xSemaphoreGive(ctx->state_mutex);
            }

            // Publish to MQTT using snapshot (outside mutex to avoid blocking)
            if (ctx->enable_mqtt) {
                mqtt_publish_chlorinator(&state_snapshot);
            }

            return true;
        }
    }

    // Internet Gateway serial number
    if (len >= sizeof(MSG_TYPE_SERIAL_NUMBER) + 6 && memcmp(data, MSG_TYPE_SERIAL_NUMBER, sizeof(MSG_TYPE_SERIAL_NUMBER)) == 0) {
        if (payload_len < 5) return false;
        // Serial number is in payload[1-4] (little endian)
        uint32_t serial = payload[1] | (payload[2] << 8) | (payload[3] << 16) | (payload[4] << 24);
        ESP_LOGI(TAG, "%s Serial number - %lu (0x%08lX)", addr_info, (unsigned long)serial, (unsigned long)serial);

        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->serial_number = serial;
            ctx->pool_state->serial_number_valid = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
        return true;
    }

    // Internet Gateway IP address
    if (len >= sizeof(MSG_TYPE_GATEWAY_IP) + 10 && memcmp(data, MSG_TYPE_GATEWAY_IP, sizeof(MSG_TYPE_GATEWAY_IP)) == 0) {
        if (payload_len < 9) return false;
        // IP address is in payload[4-7], signal level at payload[8]
        uint8_t ip[4];
        ip[0] = payload[4];
        ip[1] = payload[5];
        ip[2] = payload[6];
        ip[3] = payload[7];
        uint8_t signal_level = payload[8];

        ESP_LOGI(TAG, "%s Internet Gateway IP - %d.%d.%d.%d, signal level: %d",
                 addr_info, ip[0], ip[1], ip[2], ip[3], signal_level);

        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            memcpy(ctx->pool_state->gateway_ip, ip, 4);
            ctx->pool_state->gateway_signal_level = signal_level;
            ctx->pool_state->gateway_ip_valid = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
        return true;
    }

    // Internet Gateway communications status
    if (len >= sizeof(MSG_TYPE_GATEWAY_COMMS) + 5 && memcmp(data, MSG_TYPE_GATEWAY_COMMS, sizeof(MSG_TYPE_GATEWAY_COMMS)) == 0) {
        if (payload_len < 3) return false;
        // Comms status is in payload[1-2] (little endian)
        uint16_t comms_status = (payload[1] << 8) | payload[2];
        const char *status_text = get_gateway_comms_status_text(comms_status);

        ESP_LOGI(TAG, "%s Internet Gateway comms status - %u (%s)", addr_info, comms_status, status_text);

        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->gateway_comms_status = comms_status;
            ctx->pool_state->gateway_comms_status_valid = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
        return true;
    }

    // Internet Gateway register read request
    if (len >= sizeof(MSG_TYPE_REGISTER_READ_REQUEST) + 2 && memcmp(data, MSG_TYPE_REGISTER_READ_REQUEST, sizeof(MSG_TYPE_REGISTER_READ_REQUEST)) == 0) {
        if (payload_len < 2) return false;
        uint8_t reg_id = payload[0];

        ESP_LOGI(TAG, "%s Register read request - 0x%02X", addr_info, reg_id);

        // No state update needed - this is just a request message
        // The controller will respond with MSG_TYPE_REGISTER_STATUS
        return true;
    }

    // Controller time/clock
    if (len >= sizeof(MSG_TYPE_CONTROLLER_TIME) + 3 && memcmp(data, MSG_TYPE_CONTROLLER_TIME, sizeof(MSG_TYPE_CONTROLLER_TIME)) == 0) {
        if (payload_len < 3) return false;
        uint8_t minutes = payload[0];
        uint8_t hours = payload[1];
        uint8_t day_of_week = payload[2]; // 0=Monday, 6=Sunday

        const char *day_name = (day_of_week < DAY_OF_WEEK_COUNT) ? DAY_OF_WEEK_NAMES[day_of_week] : "Unknown";
        ESP_LOGI(TAG, "%s Controller time - %02d:%02d %s", addr_info, hours, minutes, day_name);

        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->controller_minutes = minutes;
            ctx->pool_state->controller_hours = hours;
            ctx->pool_state->controller_day_of_week = day_of_week;
            ctx->pool_state->controller_time_valid = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
        return true;
    }

    // Touchscreen firmware version
    if (len >= sizeof(MSG_TYPE_TOUCHSCREEN_VERSION) + 2 && memcmp(data, MSG_TYPE_TOUCHSCREEN_VERSION, sizeof(MSG_TYPE_TOUCHSCREEN_VERSION)) == 0) {
        if (payload_len < 2) return false;
        uint8_t major = payload[0];
        uint8_t minor = payload[1];

        ESP_LOGI(TAG, "%s Touchscreen firmware version - %d.%d", addr_info, major, minor);

        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->touchscreen_version_major = major;
            ctx->pool_state->touchscreen_version_minor = minor;
            ctx->pool_state->touchscreen_version_valid = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
        return true;
    }

    return false;
}
