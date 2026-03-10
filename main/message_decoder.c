#include "message_decoder.h"
#include "config.h"
#include "mqtt_publish.h"
#include "register_requester.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "MSG_DECODER";

// ======================================================
// Little-endian byte-to-word conversion helpers
// ======================================================

// Read 16-bit little-endian value from buffer at offset
#define UINT16_LE(ptr, offset) ((uint16_t)((ptr)[(offset)] | ((ptr)[(offset)+1] << 8)))

// Read 32-bit little-endian value from buffer at offset
#define UINT32_LE(ptr, offset) ((uint32_t)((ptr)[(offset)] | ((ptr)[(offset)+1] << 8) | \
                                            ((ptr)[(offset)+2] << 16) | ((ptr)[(offset)+3] << 24)))

// ======================================================
// Helper function for pattern matching
// ======================================================

/**
 * Match data against a hex string pattern (e.g. "02 00 50 FF FF")
 * Returns true if data matches pattern
 */
static bool match_pattern(const uint8_t *data, int data_len, const char *pattern)
{
    int data_idx = 0;
    const char *p = pattern;

    while (*p && data_idx < data_len) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        // Parse two hex digits
        if (!*p || !*(p + 1)) return false;  // Need 2 hex digits

        char hex[3] = {p[0], p[1], 0};
        unsigned long expected = strtoul(hex, NULL, 16);

        if (data[data_idx] != (uint8_t)expected) {
            return false;
        }

        data_idx++;
        p += 2;
    }

    return true;  // All pattern bytes matched
}

// ======================================================
// Message type patterns (as hex strings for readability)
// ======================================================

// Message type patterns (messages start with 0x02, end with 0x03)
// 50 Main Controller (Connect 10)
static const char *MSG_TYPE_REGISTER =              "02 00 50 FF FF 80 00 38"; 
static const char *MSG_TYPE_TEMP_SETTING =          "02 00 50 FF FF 80 00 17 10 F7";
static const char *MSG_TYPE_CONFIG =                "02 00 50 FF FF 80 00 26 0E 04";
static const char *MSG_TYPE_MODE =                  "02 00 50 FF FF 80 00 14 0D F1";
static const char *MSG_TYPE_CHANNELS =              "02 00 50 00 6F 80 00 0D 0D 5B";
static const char *MSG_TYPE_CHANNEL_STATUS =        "02 00 50 FF FF 80 00 0B 25 00";
static const char *MSG_TYPE_LIGHT_CONFIG =          "02 00 50 FF FF 80 00 06 0E E4";
static const char *MSG_TYPE_CONTROLLER_TIME =       "02 00 50 FF FF 80 00 FD 0F DC";
static const char *MSG_TYPE_TOUCHSCREEN_VERSION =   "02 00 50 FF FF 80 00 0A 0E E8";
static const char *MSG_TYPE_TOUCHSCREEN_UNKNOWN1 =  "02 00 50 FF FF 80 00 12 0E F0";
static const char *MSG_TYPE_TOUCHSCREEN_UNKNOWN2 =  "02 00 50 FF FF 80 00 27 0D 04";
static const char *MSG_TYPE_TOUCHSCREEN_UNKNOWN3 =  "02 00 50 FF FF 80 00 05 0D E2";
static const char *MSG_TYPE_VALVE_STATE =           "02 00 50 FF FF 80 00 27 13 0A";

// 62 Temperature sensor / Unknown subsystem
static const char *MSG_TYPE_TEMP_READING =          "02 00 62 FF FF 80 00 16 0E 06";
static const char *MSG_TYPE_TEMP_READING2 =         "02 00 62 FF FF 80 00 31 0E 21";
static const char *MSG_TYPE_HEATER =                "02 00 62 FF FF 80 00 12 0F 03";

// 90 Chlorinator (pH, ORP)
static const char *MSG_TYPE_CHLOR = "02 00 90 FF FF 80 00";

// Chlorinator sub-type patterns (bytes 7-10)
static const char *CHLOR_PH_SETPOINT  = "1D 0F 3C 01";
static const char *CHLOR_ORP_SETPOINT = "1D 0F 3C 02";
static const char *CHLOR_PH_READING   = "1F 0F 3E 01";
static const char *CHLOR_ORP_READING  = "1F 0F 3E 02";

// F0 Internet Gateway
static const char *MSG_TYPE_SERIAL_NUMBER =           "02 00 F0 FF FF 80 00 37 11 B8";
static const char *MSG_TYPE_GATEWAY_IP =              "02 00 F0 FF FF 80 00 37 15 BC";
static const char *MSG_TYPE_GATEWAY_COMMS =           "02 00 F0 FF FF 80 00 37 0F B6";
static const char *MSG_TYPE_GATEWAY_VERSION =         "02 00 F0 FF FF 80 00 0A 0E 88";
static const char *MSG_TYPE_GATEWAY_STATUS =          "02 00 F0 FF FF 80 00 12 0F 91";
static const char *MSG_TYPE_REGISTER_READ_REQUEST =   "02 00 F0 FF FF 80 00 39 0E B7";

// F0 Gateway Control Commands (Gateway -> Controller)
static const char *MSG_TYPE_CHANNEL_TOGGLE_CMD =      "02 00 F0 FF FF 80 00 10 0D 8D";
static const char *MSG_TYPE_TEMP_SET_CMD =            "02 00 F0 FF FF 80 00 19 0F 98";
static const char *MSG_TYPE_LIGHT_CONTROL_CMD =       "02 00 F0 FF FF 80 00 3A 0F B9";
static const char *MSG_TYPE_MODE_CONTROL_CMD =        "02 00 F0 00 50 80 00 2A 0D F9";

// ======================================================
// Lookup tables and constants
// ======================================================

// Channel type lookup table
typedef struct {
    uint8_t code;
    const char *name;
} channel_type_entry_t;

static const channel_type_entry_t CHANNEL_TYPE_TABLE[] = {
    {0x00, "Unused"},
    {0x01, "Filter"},
    {0x02, "Cleaning"},
    {0x03, "Heater Pump"},
    {0x04, "Booster"},
    {0x05, "Waterfall"},
    {0x06, "Fountain"},
    {0x07, "Spa Pump"},
    {0x08, "Solar"},
    {0x09, "Blower"},
    {0x0A, "Swimjet"},
    {0x0B, "Jets"},
    {0x0C, "Spa Jets"},
    {0x0D, "Overflow"},
    {0x0E, "Spillway"},
    {0x0F, "Audio"},
    {0x10, "Hot Seat"},
    {0x11, "Heater Power"},
    {0x12, "Custom Name"},
    {0xFD, "Heater"},
    {0xFE, "Light Zone"},
};

#define CHANNEL_TYPE_TABLE_SIZE (sizeof(CHANNEL_TYPE_TABLE) / sizeof(CHANNEL_TYPE_TABLE[0]))

/**
 * Get channel type name from type code
 * @param type_code Channel type code (0x00-0x12, 0xFD, 0xFE)
 * @return Channel type name, or "Unknown" if not found
 */
const char* get_channel_type_name(uint8_t type_code) {
    for (int i = 0; i < CHANNEL_TYPE_TABLE_SIZE; i++) {
        if (CHANNEL_TYPE_TABLE[i].code == type_code) {
            return CHANNEL_TYPE_TABLE[i].name;
        }
    }
    return "Unknown";
}

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

// Lighting zone preset name lookup table
const char *LIGHT_ZONE_NAME_TABLE[] = {
    "Pool",         // 0x00
    "Spa",          // 0x01
    "Pool & Spa",   // 0x02
    "Waterfall 1",  // 0x03
    "Waterfall 2",  // 0x04
    "Waterfall 3",  // 0x05
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

// Callback type for state update functions
typedef void (*state_update_fn)(pool_state_t *state, void *data);

/**
 * Update pool state with mutex protection and optionally publish to MQTT
 *
 * @param ctx Message decoder context
 * @param update_fn Function to update state (called with mutex held)
 * @param update_data Data to pass to update function
 * @param publish_fn MQTT publish function to call (or NULL for no publishing)
 * @return true if state was updated successfully
 */
static bool update_state_and_publish(
    message_decoder_context_t *ctx,
    state_update_fn update_fn,
    void *update_data,
    void (*publish_fn)(const pool_state_t*))
{
    if (!ctx || !ctx->state_mutex) {
        return false;
    }

    pool_state_t snapshot;
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        // Call update function with mutex held
        update_fn(ctx->pool_state, update_data);

        // Update timestamp
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Take snapshot before releasing mutex
        snapshot = *ctx->pool_state;

        xSemaphoreGive(ctx->state_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex for state update");
        return false;
    }

    // Publish to MQTT (outside mutex to avoid blocking)
    if (ctx->enable_mqtt && publish_fn) {
        publish_fn(&snapshot);
    }

    return true;
}

/**
 * Update pool state without MQTT publishing
 *
 * @param ctx Message decoder context
 * @param update_fn Function to update state (called with mutex held)
 * @param update_data Data to pass to update function
 * @return true if state was updated successfully
 */
static bool update_state_only(
    message_decoder_context_t *ctx,
    state_update_fn update_fn,
    void *update_data)
{
    return update_state_and_publish(ctx, update_fn, update_data, NULL);
}

const char* get_device_name(uint8_t addr_hi, uint8_t addr_lo)
{
    if (addr_hi == 0xFF && addr_lo == 0xFF) return "Broadcast";
    if (addr_hi == 0x00) {
        switch (addr_lo) {
            case 0x50: return "Touch Screen";
            case 0x62: return "Temp Sensor";
            case 0x90: return "Chlorinator";
            case 0x6F: return "Controller";
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
// Message handler functions
// ======================================================

/**
 * Message handler function signature
 * Returns true if message was handled successfully
 */
typedef bool (*message_handler_fn)(
    const uint8_t *data,
    int len,
    const uint8_t *payload,
    int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx);

/**
 * Register handler dispatch table entry
 * Maps (register_range, slot) to handler function
 */
typedef struct {
    uint8_t reg_start;      // Start of register range
    uint8_t reg_end;        // End of register range (inclusive)
    uint8_t slot;           // Data slot identifier
    message_handler_fn handler;
    const char *name;       // For logging
} register_handler_t;

// Forward declarations for register handlers
static bool handle_timer(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_type(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_name(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_color(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_active(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_multicolor(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_name(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_label(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_register_label_generic(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_temp_setpoint(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_count(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_touchscreen_unknown3(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);

/**
 * Register message dispatch table
 * Entries are checked in order, first match wins
 * Only includes register ranges with confirmed behavior
 */
static const register_handler_t REGISTER_HANDLERS[] = {
    // Timers (slot 0x04, registers 0x08-0x17 = timers 1-16)
    {0x08, 0x17, 0x04, handle_timer,              "Timer"},

    // Channel configuration
    {0x6C, 0x73, 0x02, handle_channel_type,       "Channel Type"},
    {0x7C, 0x83, 0x02, handle_channel_name,       "Channel Name"},
    {0x8C, 0x93, 0x02, handle_channel_state,      "Channel State"},

    // Lighting zones
    {0xA0, 0xA7, 0x01, handle_light_zone_multicolor, "Light Zone Multicolor"},
    {0xB0, 0xB7, 0x01, handle_light_zone_name,    "Light Zone Name"},
    {0xC0, 0xC7, 0x01, handle_light_zone_state,   "Light Zone State"},
    {0xD0, 0xD7, 0x01, handle_light_zone_color,   "Light Zone Color"},
    {0xE0, 0xE7, 0x01, handle_light_zone_active,  "Light Zone Active"},

    // Valve labels (slot 0x02)
    {0xD0, 0xD1, 0x02, handle_valve_label,        "Valve Label"},

    // Labels (slot 0x03) - only specific ranges we've observed
    {0x31, 0x38, 0x03, handle_register_label_generic, "Favourite Label"},

    // Temperature setpoints (slot 0x00, registers 0xE7=Pool, 0xE8=Spa)
    {0xE7, 0xE8, 0x00, handle_temp_setpoint,          "Temperature Setpoint"},
    {0xF4, 0xF4, 0x01, handle_channel_count,          "Channel Count"},
};

#define REGISTER_HANDLER_COUNT (sizeof(REGISTER_HANDLERS) / sizeof(REGISTER_HANDLERS[0]))

/**
 * Handler: Temperature reading message
 * Pattern: "02 00 62 FF FF 80 00 16 0E"
 */
static bool handle_temp_reading(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t current_temp = payload[0];
    ESP_LOGI(TAG, "%s Current temperature - %d", addr_info, current_temp);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for temp reading");
        return true;
    }
    ctx->pool_state->current_temp = current_temp;
    ctx->pool_state->temp_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_temperature(&snapshot);
    }

    return true;
}

/**
 * Handler: Temperature setpoint register messages
 * Register 0xE7 (Pool), 0xE8 (Spa), Slot 0x00
 */
static bool handle_temp_setpoint(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t temp_c = payload[2];
    bool is_pool = (reg_id == 0xE7);

    ESP_LOGI(TAG, "%s %s temperature setpoint - %d°C", addr_info,
             is_pool ? "Pool" : "Spa", temp_c);

    pool_state_t state_snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for temp setpoint");
        return true;
    }
    if (is_pool) {
        ctx->pool_state->pool_setpoint = temp_c;
    } else {
        ctx->pool_state->spa_setpoint = temp_c;
    }
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    state_snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_temperature(&state_snapshot);
    }

    return true;
}

/**
 * Handler: Channel count
 * Register 0xF4, Slot 0x01
 * Reports the total number of channels configured in the system.
 */
static bool handle_channel_count(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t count = payload[2];
    if (count > MAX_CHANNELS) count = MAX_CHANNELS;

    ESP_LOGI(TAG, "%s Channel count - %d", addr_info, count);

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->num_channels = count;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Temperature reading (variant 2) message
 * Pattern: "02 00 62 FF FF 80 00 31 0E 21"
 *
 * Same temperature data as MSG_TYPE_TEMP_READING but a different command byte (0x31
 * vs 0x16). Byte 11 is always 0xA6 in observed samples — purpose unknown.
 */
static bool handle_temp_reading2(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t current_temp = payload[0];
    uint8_t unknown      = payload[1];
    ESP_LOGI(TAG, "%s Current temperature - %d°C (unknown byte: 0x%02X)", addr_info, current_temp, unknown);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for temp reading2");
        return true;
    }
    ctx->pool_state->current_temp = current_temp;
    ctx->pool_state->temp_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_temperature(&snapshot);
    }

    return true;
}

/**
 * Handler: Heater status message
 * Pattern: "02 00 62 FF FF 80 00 12 0F"
 */
static bool handle_heater(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t heater_state = payload[1];
    ESP_LOGI(TAG, "%s Heater - %s", addr_info, heater_state ? "On" : "Off");

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for heater");
        return true;
    }
    ctx->pool_state->heaters[0].on = (heater_state != 0);
    ctx->pool_state->heaters[0].valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_heater(&snapshot, 0);
    }

    return true;
}

/**
 * Handler: Temperature setting message
 * Pattern: "02 00 50 FF FF 80 00 17 10 F7"
 */
static bool handle_temp_setting(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 4) return false;

    uint8_t spa_set_temp_c = payload[0];
    uint8_t pool_set_temp_c = payload[1];
    uint8_t spa_set_temp_f = payload[2];
    uint8_t pool_set_temp_f = payload[3];

    ESP_LOGI(TAG, "%s Temperature settings - spa=%d°C/%d°F, pool=%d°C/%d°F",
             addr_info, spa_set_temp_c, spa_set_temp_f, pool_set_temp_c, pool_set_temp_f);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for temp setting");
        return true;
    }
    ctx->pool_state->spa_setpoint = spa_set_temp_c;
    ctx->pool_state->pool_setpoint = pool_set_temp_c;
    ctx->pool_state->spa_setpoint_f = spa_set_temp_f;
    ctx->pool_state->pool_setpoint_f = pool_set_temp_f;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_temperature(&snapshot);
    }

    return true;
}

/**
 * Handler: Mode message (Spa/Pool)
 * Pattern: "02 00 50 FF FF 80 00 14 0D F1"
 */
static bool handle_mode(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t mode = payload[0];
    const char *mode_str = (mode == 0x00) ? "Spa" : (mode == 0x01) ? "Pool" : "Unknown";
    ESP_LOGI(TAG, "%s Mode - %s", addr_info, mode_str);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for mode");
        return true;
    }
    ctx->pool_state->mode = mode;
    ctx->pool_state->mode_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_mode(&snapshot);
    }

    return true;
}

/**
 * Handler: Configuration message
 * Pattern: "02 00 50 FF FF 80 00 26 0E 04"
 */
static bool handle_config(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t config_byte = payload[0];
    const char *scale_str = (config_byte & 0x10) ? "Fahrenheit" : "Celsius";
    ESP_LOGI(TAG, "%s Config - temperature scale=%s", addr_info, scale_str);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->temp_scale_fahrenheit = (config_byte & 0x10) != 0;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return false;  // Intentionally return false to match original behavior
}

/**
 * Handler: Controller time/clock message
 * Pattern: "02 00 50 FF FF 80 00 FD 0F DC"
 */
static bool handle_controller_time(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t minutes = payload[0];
    uint8_t hours = payload[1];
    uint8_t day_of_week = payload[2];  // 0=Monday, 6=Sunday

    const char *day_name = (day_of_week < DAY_OF_WEEK_COUNT) ? DAY_OF_WEEK_NAMES[day_of_week] : "Unknown";
    ESP_LOGI(TAG, "%s Controller time - %02d:%02d %s", addr_info, hours, minutes, day_name);

    // Update state only (no MQTT publishing)
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

/**
 * Handler: Touchscreen firmware version message
 * Pattern: "02 00 50 FF FF 80 00 0A 0E E8"
 */
static bool handle_touchscreen_version(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t major = payload[0];
    uint8_t minor = payload[1];

    ESP_LOGI(TAG, "%s Touchscreen firmware version - %d.%d", addr_info, major, minor);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->touchscreen_version_major = major;
        ctx->pool_state->touchscreen_version_minor = minor;
        ctx->pool_state->touchscreen_version_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Touchscreen other status info message
 * Pattern: "02 00 50 FF FF 80 00 12 0E F0"
 */
static bool handle_touchscreen_unknown1(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t data_byte1 = payload[0];
    uint8_t data_byte2 = payload[1];

    if (data_byte1 != 0x05 || data_byte2 != 0x00) {
        ESP_LOGW(TAG, "%s Touchscreen other status - UNEXPECTED VALUE: Byte1: 0x%02X (%d), Byte2: 0x%02X (%d) (expected 0x05 0x00)",
                 addr_info, data_byte1, data_byte1, data_byte2, data_byte2);
    } else {
        ESP_LOGI(TAG, "%s Touchscreen other status - Byte1: 0x%02X (%d), Byte2: 0x%02X (%d)",
                 addr_info, data_byte1, data_byte1, data_byte2, data_byte2);
    }

    return true;
}

/**
 * Handler: Unknown/unhandled message
 * Logs the raw hex bytes for messages that don't match any known pattern
 */
static bool handle_unknown(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    // Format message as hex string
    int hex_str_size = 3 * len + 1;
    char *hex_str = malloc(hex_str_size);
    if (!hex_str) return false;
    int pos = 0;
    for (int i = 0; i < len; i++) {
        pos += snprintf(&hex_str[pos], hex_str_size - pos, "%02X ", data[i]);
    }
    hex_str[pos] = '\0';

    ESP_LOGW(TAG, "Unhandled: %s", hex_str);
    free(hex_str);

    return false;  // Not decoded
}

/**
 * Handler: Controller status message
 * Pattern: "02 00 50 FF FF 80 00 27 0D 04"
 */
static bool handle_touchscreen_unknown2(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    // Short form of valve state broadcast (startup / no valve state available)
    ESP_LOGI(TAG, "%s Valve state broadcast (startup form)", addr_info);
    return true;
}

/**
 * Handler: Touchscreen unknown broadcast (CMD 0x05)
 * Invariant across all captures: data byte always 0x01.
 * Silenced here to avoid spurious "Unhandled" warnings.
 */
static bool handle_touchscreen_unknown3(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    ESP_LOGD(TAG, "%s Touchscreen unknown (CMD 0x05): 0x%02X", addr_info,
             payload_len > 0 ? payload[0] : 0);
    return true;
}

/**
 * Handler: Valve state broadcast (long form)
 * Pattern: "02 00 50 FF FF 80 00 27 13 0A"
 * Byte 10: slot count; then 3 bytes per slot: [configured][state][active]
 */
static bool handle_valve_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t slot_count = payload[0];
    if (slot_count > MAX_VALVE_SLOTS) slot_count = MAX_VALVE_SLOTS;

    if (payload_len < 1 + slot_count * 3) {
        ESP_LOGW(TAG, "%s Valve state - truncated payload (slots=%d, payload_len=%d)",
                 addr_info, slot_count, payload_len);
        return true;
    }

    // Log each configured valve before taking the mutex
    for (int i = 0; i < slot_count; i++) {
        bool configured = (payload[1 + i * 3] == 0x01);
        uint8_t state   = payload[2 + i * 3];
        bool active     = (payload[3 + i * 3] == 0x01);
        if (configured) {
            const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
            ESP_LOGI(TAG, "%s Valve %d - %s (%s)", addr_info, i + 1,
                     state_name, active ? "Active" : "Inactive");
        }
    }

    bool changed = false;
    bool new_valve_configured = false;
    pool_state_t state_snapshot;
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->num_valve_slots = slot_count;
        for (int i = 0; i < slot_count; i++) {
            bool configured = (payload[1 + i * 3] == 0x01);
            uint8_t state   = payload[2 + i * 3];
            bool active     = (payload[3 + i * 3] == 0x01);
            valve_state_t *v = &ctx->pool_state->valves[i];
            if (!v->configured && configured) {
                new_valve_configured = true;
            }
            if (v->configured != configured || v->state != state || v->active != active) {
                v->configured = configured;
                v->state      = state;
                v->active     = active;
                changed = true;
            }
        }
        if (changed) {
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        }
        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    // Wake the register requester to fetch the label for any newly seen valve
    if (new_valve_configured) {
        register_requester_notify();
    }

    if (changed && ctx->enable_mqtt) {
        for (int i = 0; i < slot_count; i++) {
            mqtt_publish_valve(&state_snapshot, i + 1);
        }
    }

    return true;
}

/**
 * Handler: Internet Gateway serial number message
 * Pattern: "02 00 F0 FF FF 80 00 37 11 B8"
 */
static bool handle_serial_number(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 5) return false;

    // Serial number is in payload[1-4] (little endian)
    uint32_t serial = UINT32_LE(payload, 1);
    ESP_LOGI(TAG, "%s Serial number - %" PRIu32 " (0x%08" PRIX32 ")", addr_info, serial, serial);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->serial_number = serial;
        ctx->pool_state->serial_number_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Internet Gateway IP address message
 * Pattern: "02 00 F0 FF FF 80 00 37 15 BC"
 */
static bool handle_gateway_ip(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
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

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        memcpy(ctx->pool_state->gateway_ip, ip, 4);
        ctx->pool_state->gateway_signal_level = signal_level;
        ctx->pool_state->gateway_ip_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Internet Gateway communications status message
 * Pattern: "02 00 F0 FF FF 80 00 37 0F B6"
 */
static bool handle_gateway_comms(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    // Comms status is in payload[1-2] (little endian)
    uint16_t comms_status = UINT16_LE(payload, 1);
    const char *status_text = get_gateway_comms_status_text(comms_status);

    ESP_LOGI(TAG, "%s Internet Gateway comms status - %u (%s)", addr_info, comms_status, status_text);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->gateway_comms_status = comms_status;
        ctx->pool_state->gateway_comms_status_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Internet Gateway firmware version
 * Pattern: "02 00 F0 FF FF 80 00 0A 0E 88"
 */
static bool handle_gateway_version(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t major = payload[0];
    uint8_t minor = payload[1];

    ESP_LOGI(TAG, "%s Internet Gateway firmware version - %d.%d", addr_info, major, minor);

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->gateway_version_major = major;
        ctx->pool_state->gateway_version_minor = minor;
        ctx->pool_state->gateway_version_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Internet Gateway status broadcast
 * Pattern: "02 00 F0 FF FF 80 00 12 0F 91"
 */
static bool handle_gateway_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    ESP_LOGI(TAG, "%s Internet Gateway status - 0x%02X 0x%02X 0x%02X",
             addr_info, payload[0], payload[1], payload[2]);

    return true;
}

/**
 * Handler: Register read request message
 * Pattern: "02 00 F0 FF FF 80 00 39 0E B7"
 */
static bool handle_register_read_request(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t reg_id = payload[0];
    uint8_t slot_id = payload[1];

    // Resolve a human-readable description by searching the register dispatch table
    char desc[48];
    bool found = false;
    for (int i = 0; i < REGISTER_HANDLER_COUNT; i++) {
        const register_handler_t *entry = &REGISTER_HANDLERS[i];
        if (reg_id >= entry->reg_start && reg_id <= entry->reg_end && entry->slot == slot_id) {
            snprintf(desc, sizeof(desc), "%s %d", entry->name, reg_id - entry->reg_start + 1);
            found = true;
            break;
        }
    }
    if (!found) {
        snprintf(desc, sizeof(desc), "0x%02X/0x%02X", reg_id, slot_id);
    }

    ESP_LOGI(TAG, "%s Register read request - %s", addr_info, desc);

    // No state update needed - this is just a request message
    return true;
}

/**
 * Handler: Channel toggle command (Gateway -> Controller)
 * Pattern: "02 00 F0 FF FF 80 00 10 0D 8D"
 */
static bool handle_channel_toggle_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t channel_idx = payload[0];
    uint8_t channel_num = channel_idx + 1;  // Convert to 1-based

    // Look up channel name from pool state
    char channel_name[32] = {0};
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (channel_idx < MAX_CHANNELS && ctx->pool_state->channels[channel_idx].configured) {
            strncpy(channel_name, ctx->pool_state->channels[channel_idx].name, sizeof(channel_name) - 1);
        }
        xSemaphoreGive(ctx->state_mutex);
    }

    if (channel_name[0] != '\0') {
        ESP_LOGI(TAG, "%s Gateway channel toggle command - Channel %d (%s)",
                 addr_info, channel_num, channel_name);
    } else {
        ESP_LOGI(TAG, "%s Gateway channel toggle command - Channel %d (index 0x%02X, name unknown)",
                 addr_info, channel_num, channel_idx);
    }

    // No state update needed - this is a command message, not status
    // The controller will respond with an updated Channel Status message
    return true;
}

/**
 * Handler: Temperature setpoint command (Gateway -> Controller)
 * Pattern: "02 00 F0 FF FF 80 00 19 0F 98"
 */
static bool handle_temp_set_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t target = payload[0];
    uint8_t temp_c = payload[1];  // Repeated at payload[2], only need one

    const char *target_name = (target == 0x01) ? "Pool" : (target == 0x02) ? "Spa" : "Unknown";
    ESP_LOGI(TAG, "%s Gateway temperature set command - %s setpoint -> %d°C",
             addr_info, target_name, temp_c);

    // No state update needed - the controller will broadcast the new setpoint
    return true;
}

/**
 * Handler: Light zone control command (Gateway -> Controller)
 * Pattern: "02 00 F0 FF FF 80 00 3A 0F B9"
 */
static bool handle_light_control_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t slot = payload[1];
    uint8_t state = payload[2];

    // Dispatch based on register ID and slot
    if (reg_id >= 0xC0 && reg_id <= 0xC7 && slot == 0x01) {
        // Light zone state control (0xC0-0xC7, slot 0x01)
        uint8_t zone_num = reg_id - 0xC0 + 1;
        const char *state_name = (state == 0x00) ? "Off" : (state == 0x01) ? "Auto" : (state == 0x02) ? "On" : "Unknown";
        ESP_LOGI(TAG, "%s Gateway light control command - Zone %d -> %s (0x%02X)",
                 addr_info, zone_num, state_name, state);
    } else if (reg_id == 0xE6 && slot == 0x00) {
        // Heater on/off control (0xE6, slot 0x00)
        ESP_LOGI(TAG, "%s Gateway heater control command - Heater -> %s",
                 addr_info, state ? "On" : "Off");
    } else {
        ESP_LOGW(TAG, "%s Gateway register write command - Unknown Reg=0x%02X, Slot=0x%02X, State=0x%02X",
                 addr_info, reg_id, slot, state);
    }

    // No state update needed - this is a command message, not status
    // The controller will respond with a register status update
    return true;
}

/**
 * Handler: Mode control command (Gateway -> Controller)
 * Pattern: "02 00 F0 00 50 80 00 2A 0D F9"
 */
static bool handle_mode_control_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t mode_value = payload[0];
    const char *mode_name = (mode_value == 0x00) ? "Pool" : (mode_value == 0x01) ? "Spa" : "Unknown";

    ESP_LOGI(TAG, "%s Gateway mode control command - Switch to %s mode (0x%02X)",
             addr_info, mode_name, mode_value);

    // No state update needed - this is a command message, not status
    // The controller will respond with a mode status update
    return true;
}

/**
 * Handler: Chlorinator pH setpoint message
 * Pattern: "02 00 90 FF FF 80 00" + sub-type "1D 0F 3C 01"
 */
static bool handle_chlor_ph_setpoint(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint16_t value = UINT16_LE(payload, 1);
    ESP_LOGI(TAG, "%s Chlorinator pH setpoint - %.1f", addr_info, value / 10.0);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for pH setpoint");
        return true;
    }
    ctx->pool_state->ph_setpoint = value;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator ORP setpoint message
 * Pattern: "02 00 90 FF FF 80 00" + sub-type "1D 0F 3C 02"
 */
static bool handle_chlor_orp_setpoint(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint16_t value = UINT16_LE(payload, 1);
    ESP_LOGI(TAG, "%s Chlorinator ORP setpoint - %d mV", addr_info, value);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for ORP setpoint");
        return true;
    }
    ctx->pool_state->orp_setpoint = value;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator pH reading message
 * Pattern: "02 00 90 FF FF 80 00" + sub-type "1F 0F 3E 01"
 */
static bool handle_chlor_ph_reading(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint16_t value = UINT16_LE(payload, 1);
    ESP_LOGI(TAG, "%s Chlorinator pH reading - %.1f", addr_info, value / 10.0);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for pH reading");
        return true;
    }
    ctx->pool_state->ph_reading = value;
    ctx->pool_state->ph_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator ORP reading message
 * Pattern: "02 00 90 FF FF 80 00" + sub-type "1F 0F 3E 02"
 */
static bool handle_chlor_orp_reading(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint16_t value = UINT16_LE(payload, 1);
    ESP_LOGI(TAG, "%s Chlorinator ORP reading - %d mV", addr_info, value);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for ORP reading");
        return true;
    }
    ctx->pool_state->orp_reading = value;
    ctx->pool_state->orp_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}


/**
 * Handler: Light configuration message
 * Pattern: "02 00 50 FF FF 80 00 06 0E E4"
 */
static bool handle_light_config(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t zone_idx  = payload[0];
    uint8_t light_on  = payload[1];

    if (zone_idx <= 3) {
        ESP_LOGI(TAG, "%s Lighting zone %d - %s", addr_info, zone_idx + 1, light_on ? "On" : "Off");

        pool_state_t state_snapshot;
        bool newly_configured = false;

        if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to acquire mutex for light config");
            return true;
        }
        newly_configured = !ctx->pool_state->lighting[zone_idx].configured;
        ctx->pool_state->lighting[zone_idx].zone       = zone_idx + 1;
        ctx->pool_state->lighting[zone_idx].configured = true;
        ctx->pool_state->lighting[zone_idx].active     = (light_on != 0);
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);

        if (newly_configured) {
            register_requester_notify();
        }

        if (ctx->enable_mqtt) {
            mqtt_publish_light(&state_snapshot, zone_idx + 1);
        }
    }

    return true;
}

// ======================================================
// Register message handlers (dispatched by register range and slot)
// ======================================================

/**
 * Handler: Timer configuration
 * Register range: 0x08-0x17 (timers 1-16), Slot: 0x04
 *
 * Payload layout (bytes within payload[], offset from byte 10):
 *   [0] reg_id       - register (0x08=timer1 .. 0x17=timer16)
 *   [1] slot         - always 0x04
 *   [2] start_hour   - 24h start hour
 *   [3] start_minute - start minute
 *   [4] stop_hour    - 24h stop hour
 *   [5] stop_minute  - stop minute
 *   [6] days         - bitmask (assumed: bit0=Mon..bit6=Sun; 0x7F=every day, 0x00=disabled)
 */
static bool handle_timer(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 7) return false;

    uint8_t reg_id       = payload[0];
    // payload[1] = slot (0x04) - not needed
    uint8_t start_hour   = payload[2];
    uint8_t start_minute = payload[3];
    uint8_t stop_hour    = payload[4];
    uint8_t stop_minute  = payload[5];
    uint8_t days         = payload[6];

    uint8_t timer_num = reg_id - 0x08 + 1;

    // Build compact day string: MTWTFSS where '-' means not set
    // Assumed mapping: bit0=Mon, bit1=Tue, bit2=Wed, bit3=Thu, bit4=Fri, bit5=Sat, bit6=Sun
    char days_str[8];
    const char day_chars[] = "MTWTFSS";
    for (int i = 0; i < 7; i++) {
        days_str[i] = (days & (1 << i)) ? day_chars[i] : '-';
    }
    days_str[7] = '\0';

    if (days == 0x00 && start_hour == 0 && start_minute == 0 && stop_hour == 0 && stop_minute == 0) {
        ESP_LOGI(TAG, "%s Timer %d - not configured", addr_info, timer_num);
    } else {
        ESP_LOGI(TAG, "%s Timer %d - start=%02d:%02d stop=%02d:%02d days=0x%02X [%s]",
                 addr_info, timer_num,
                 start_hour, start_minute, stop_hour, stop_minute,
                 days, days_str);
    }

    // Log any extra bytes beyond the 7 known bytes (for future decoding)
    if (payload_len > 7) {
        int extra = payload_len - 7;
        char extra_hex[64] = {0};
        int pos = 0;
        for (int i = 7; i < payload_len && pos < (int)sizeof(extra_hex) - 4; i++) {
            pos += snprintf(&extra_hex[pos], sizeof(extra_hex) - pos, "%02X ", payload[i]);
        }
        ESP_LOGW(TAG, "%s Timer %d - %d extra unknown byte(s): %s", addr_info, timer_num, extra, extra_hex);
    }

    // Update state
    if (timer_num >= 1 && timer_num <= MAX_TIMERS) {
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            int idx = timer_num - 1;
            ctx->pool_state->timers[idx].timer_num    = timer_num;
            ctx->pool_state->timers[idx].start_hour   = start_hour;
            ctx->pool_state->timers[idx].start_minute = start_minute;
            ctx->pool_state->timers[idx].stop_hour    = stop_hour;
            ctx->pool_state->timers[idx].stop_minute  = stop_minute;
            ctx->pool_state->timers[idx].days         = days;
            ctx->pool_state->timers[idx].valid        = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    return true;
}

/**
 * Handler: Channel type configuration
 * Register range: 0x6C-0x73, Slot: 0x02
 */
static bool handle_channel_type(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t ch_type = payload[2];
    uint8_t ch_num = reg_id - 0x6C + 1;

    const char *type_name = get_channel_type_name(ch_type);
    ESP_LOGI(TAG, "%s Channel %d type - %s (%d)", addr_info, ch_num, type_name, ch_type);

    if (ch_type != CHANNEL_UNUSED) {
        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->channels[ch_num - 1].type = ch_type;
            ctx->pool_state->channels[ch_num - 1].configured = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    return true;
}

/**
 * Handler: Channel names
 * Register range: 0x7C-0x83, Slot: 0x02
 */
static bool handle_channel_name(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    // Need at least 3 bytes: register ID, slot, and name data (even if null terminator)
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t ch_num = reg_id - 0x7C + 1;

    // Safely copy string from payload — protocol does not guarantee null termination
    char name[32] = {0};
    int str_len = payload_len - 2;
    if (str_len > (int)sizeof(name) - 1) str_len = (int)sizeof(name) - 1;
    memcpy(name, &payload[2], str_len);

    // Check if it's an empty/unused channel (first byte is 0x00)
    if (name[0] == '\0') {
        ESP_LOGI(TAG, "%s Channel %d name - (empty)", addr_info, ch_num);
    } else {
        ESP_LOGI(TAG, "%s Channel %d name - \"%s\"", addr_info, ch_num, name);

        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            if (ch_num <= MAX_CHANNELS) {
                strncpy(ctx->pool_state->channels[ch_num - 1].name, name, sizeof(ctx->pool_state->channels[ch_num - 1].name) - 1);
                ctx->pool_state->channels[ch_num - 1].id = ch_num;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            }
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    return true;
}

/**
 * Handler: Channel state (read-only broadcast)
 * Register range: 0x8C-0x93, Slot: 0x02
 * Values: 0x00=Off, 0x01=Auto, 0x02=On
 * Note: write commands (0x3A) targeting these registers are silently ignored by the controller.
 *       Use the Channel Toggle Command to change channel state.
 */
static bool handle_channel_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t state  = payload[2];
    uint8_t ch_num = reg_id - 0x8C + 1;

    const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
    ESP_LOGI(TAG, "%s Channel %d state - %s", addr_info, ch_num, state_name);

    if (ch_num > MAX_CHANNELS) return true;

    pool_state_t state_snapshot;
    bool changed = false;
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        channel_state_t *ch = &ctx->pool_state->channels[ch_num - 1];
        if (!ch->configured || ch->state != state) {
            ch->state = state;
            ch->configured = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            changed = true;
        }
        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (changed && ctx->enable_mqtt) {
        mqtt_publish_channel(&state_snapshot, ch_num);
    }

    return true;
}

/**
 * Handler: Lighting zone state
 * Register range: 0xC0-0xC7, Slot: 0x01
 */
static bool handle_light_zone_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t state = payload[2];
    uint8_t zone_idx = reg_id - 0xC0;

    const char *state_name = (state < LIGHTING_STATE_COUNT) ? LIGHTING_STATE_NAMES[state] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d state - %s", addr_info, zone_idx + 1, state_name);

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].zone = zone_idx + 1;
        ctx->pool_state->lighting[zone_idx].state = state;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (should_publish && ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Lighting zone color
 * Register range: 0xD0-0xD7, Slot: 0x01
 */
static bool handle_light_zone_color(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t color = payload[2];
    uint8_t zone_idx = reg_id - 0xD0;

    const char *color_name = (color < LIGHTING_COLOR_COUNT) ? LIGHTING_COLOR_NAMES[color] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d color - %s (%d)", addr_info, zone_idx + 1, color_name, color);

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].color = color;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (should_publish && ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Lighting zone multicolor capability
 * Register range: 0xA0-0xA7, Slot: 0x01
 */
static bool handle_light_zone_multicolor(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t capable = payload[2];
    uint8_t zone_idx = reg_id - 0xA0;

    ESP_LOGI(TAG, "%s Lighting zone %d multicolor - %s", addr_info, zone_idx + 1, capable ? "Yes" : "No");

    pool_state_t state_snapshot;
    bool should_publish = false;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].multicolor = (capable != 0);
        ctx->pool_state->lighting[zone_idx].multicolor_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state_snapshot = *ctx->pool_state;
        should_publish = ctx->pool_state->lighting[zone_idx].configured;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (ctx->enable_mqtt && should_publish) {
        mqtt_publish_light(&state_snapshot, zone_idx + 1);
    }

    return true;
}

/**
 * Handler: Lighting zone preset name
 * Register range: 0xB0-0xB7, Slot: 0x01
 */
static bool handle_light_zone_name(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t name_id = payload[2];
    uint8_t zone_idx = reg_id - 0xB0;

    const char *name = (name_id < LIGHT_ZONE_NAME_COUNT) ? LIGHT_ZONE_NAME_TABLE[name_id] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d name - %s (%d)", addr_info, zone_idx + 1, name, name_id);

    pool_state_t state_snapshot;
    bool should_publish = false;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].name_id = name_id;
        ctx->pool_state->lighting[zone_idx].name_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state_snapshot = *ctx->pool_state;
        should_publish = ctx->pool_state->lighting[zone_idx].configured;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (ctx->enable_mqtt && should_publish) {
        mqtt_publish_light(&state_snapshot, zone_idx + 1);
    }

    return true;
}

/**
 * Handler: Lighting zone active state
 * Register range: 0xE0-0xE7, Slot: 0x01
 */
static bool handle_light_zone_active(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t active = payload[2];
    uint8_t zone_idx = reg_id - 0xE0;

    ESP_LOGI(TAG, "%s Lighting zone %d active - %s", addr_info, zone_idx + 1, active ? "Yes" : "No");

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].active = (active != 0);
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (should_publish && ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Valve labels
 * Register range: 0xD0-0xD1, Slot: 0x02
 */
static bool handle_valve_label(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t zone_num = reg_id - 0xD0 + 1;

    // Safely copy string from payload — protocol does not guarantee null termination
    char label[32] = {0};
    int str_len = payload_len - 2;
    if (str_len > (int)sizeof(label) - 1) str_len = (int)sizeof(label) - 1;
    memcpy(label, &payload[2], str_len);

    ESP_LOGI(TAG, "%s Valve zone %d label (0x%02X) - \"%s\"", addr_info, zone_num, reg_id, label);

    // Update pool state
    pool_state_t state_snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for valve label");
        return true;
    }
    int slot = -1;
    for (int i = 0; i < MAX_REGISTER_LABELS; i++) {
        if (ctx->pool_state->register_labels[i].valid && ctx->pool_state->register_labels[i].reg_id == reg_id) {
            slot = i;
            break;
        } else if (!ctx->pool_state->register_labels[i].valid && slot == -1) {
            slot = i;
        }
    }

    if (slot >= 0) {
        ctx->pool_state->register_labels[slot].reg_id = reg_id;
        strncpy(ctx->pool_state->register_labels[slot].label, label, sizeof(ctx->pool_state->register_labels[slot].label) - 1);
        ctx->pool_state->register_labels[slot].label[sizeof(ctx->pool_state->register_labels[slot].label) - 1] = '\0';
        ctx->pool_state->register_labels[slot].valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }

    // Also store directly in valve state for MQTT name-change detection
    int valve_idx = reg_id - 0xD0;
    if (valve_idx >= 0 && valve_idx < MAX_VALVE_SLOTS) {
        strncpy(ctx->pool_state->valves[valve_idx].name, label,
                sizeof(ctx->pool_state->valves[valve_idx].name) - 1);
        ctx->pool_state->valves[valve_idx].name[sizeof(ctx->pool_state->valves[valve_idx].name) - 1] = '\0';
    }

    state_snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    // Re-publish valve discovery and state now that the name is known
    if (ctx->enable_mqtt && zone_num >= 1 && zone_num <= MAX_VALVE_SLOTS) {
        mqtt_publish_valve(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Generic register label (catch-all for other label types)
 * Register range: 0x00-0xFF, Slot: 0x03
 */
static bool handle_register_label_generic(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];

    // Safely copy string from payload — protocol does not guarantee null termination
    char label[32] = {0};
    int str_len = payload_len - 2;
    if (str_len > (int)sizeof(label) - 1) str_len = (int)sizeof(label) - 1;
    memcpy(label, &payload[2], str_len);

    ESP_LOGI(TAG, "%s Register 0x%02X label - \"%s\"", addr_info, reg_id, label);

    // Update pool state
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        int slot = -1;
        for (int i = 0; i < MAX_REGISTER_LABELS; i++) {
            if (ctx->pool_state->register_labels[i].valid && ctx->pool_state->register_labels[i].reg_id == reg_id) {
                slot = i;
                break;
            } else if (!ctx->pool_state->register_labels[i].valid && slot == -1) {
                slot = i;
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


/**
 * Handler: Active channels bitmask message
 * Pattern: "02 00 50 00 6F 80 00 0D 0D 5B"
 */
static bool handle_channels(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
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

/**
 * Handler: Channel status message (most complex handler)
 * Pattern: "02 00 50 FF FF 80 00 0B 25 00"
 */
static bool handle_channel_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t num_channels = payload[0];
    if (num_channels > MAX_CHANNELS) {
        ESP_LOGW(TAG, "%s Channel count %d exceeds MAX_CHANNELS (%d), clamping",
                 addr_info, num_channels, MAX_CHANNELS);
        num_channels = MAX_CHANNELS;
    }
    ESP_LOGI(TAG, "%s Channel status (%d channels):", addr_info, num_channels);

    int payload_idx = 1;  // Channel data starts at payload[1]
    int ch_num = 1;
    bool past_end = false;
    uint8_t channels_to_publish[8] = {0};
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
            uint8_t state   = payload[payload_idx + 1];
            uint8_t active  = payload[payload_idx + 2];
            const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";

            if (ch_type == CHANNEL_UNUSED) {
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                ctx->pool_state->channels[ch_num - 1].configured = false;
            } else {
                const char *type_name = get_channel_type_name(ch_type);
                ESP_LOGI(TAG, "  Ch%d: %s (%d) = %s (%s)", ch_num, type_name, ch_type, state_name,
                         active ? "Active" : "Inactive");

                // Update channel state
                ctx->pool_state->channels[ch_num - 1].id = ch_num;
                ctx->pool_state->channels[ch_num - 1].type = ch_type;
                ctx->pool_state->channels[ch_num - 1].state = state;
                ctx->pool_state->channels[ch_num - 1].active = (active != 0);
                ctx->pool_state->channels[ch_num - 1].configured = true;

                // Mark this channel for publishing
                channels_to_publish[num_to_publish++] = ch_num;
            }

            payload_idx += 3;
            ch_num++;
        }

        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    // Publish all channels using snapshot (outside mutex)
    if (ctx->enable_mqtt) {
        for (int i = 0; i < num_to_publish; i++) {
            mqtt_publish_channel(&state_snapshot, channels_to_publish[i]);
        }
    }

    return true;
}

// ======================================================
// Main decoder function
// ======================================================

bool decode_message(const uint8_t *data, int len, message_decoder_context_t *ctx)
{
    if (!ctx || !ctx->pool_state) {
        return false;
    }

    // Minimum valid message: 10-byte header + data checksum + end byte
    if (len < 12 || data[0] != 0x02 || data[len - 1] != 0x03) {
        return false;
    }

    // Log full message before decoding
    int full_msg_size = 3 * len + 1;
    char *full_msg = malloc(full_msg_size);
    if (!full_msg) return false;
    int msg_pos = 0;
    for (int i = 0; i < len && msg_pos < full_msg_size - 3; i++) {
        msg_pos += snprintf(&full_msg[msg_pos], full_msg_size - msg_pos, "%02X ", data[i]);
    }
    full_msg[msg_pos] = '\0';
    ESP_LOGI(TAG, "RX MSG: %s", full_msg);
    free(full_msg);

    // Validate length field: byte[8] = total message length including START and END
    if (data[8] != len) {
        ESP_LOGW(TAG, "Length field mismatch: byte[8]=0x%02X (%d), actual=%d",
                 data[8], data[8], len);
    }

    // Validate header checksum: byte[9] = sum(bytes 0-8) & 0xFF
    uint8_t expected_hchk = 0;
    for (int i = 0; i < 9; i++) expected_hchk += data[i];
    if (expected_hchk != data[9]) {
        ESP_LOGW(TAG, "Header checksum FAILED: expected 0x%02X, got 0x%02X",
                 expected_hchk, data[9]);
    }

    // Validate data checksum
    if (!verify_message_checksum(data, len)) {
        uint32_t sum = 0;
        for (int i = 10; i < len - 2; i++) sum += data[i];
        ESP_LOGW(TAG, "Data checksum FAILED: expected 0x%02X, got 0x%02X",
                 (uint8_t)(sum & 0xFF), data[len - 2]);
    } else {
        ESP_LOGD(TAG, "Checksums OK");
    }

    // Extract data payload section (bytes 10 to len-3)
    // Message format: [START=0][SRC=1-2][DST=3-4][CTRL=5-6][CMD=7][LEN=8][HDR_CHK=9][DATA=10...][DATA_CHK=len-2][END=len-1]
    const uint8_t *payload = &data[10];
    int payload_len = len - 12;  // len - (10 header bytes + data checksum + end)

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

    // Dispatch to message handlers

    // Register messages (dispatch table approach)
    if (match_pattern(data, len, MSG_TYPE_REGISTER)) {
        // Header checksum already validated above

        // Extract register ID and slot
        if (payload_len < 2) {
            ESP_LOGW(TAG, "%s Register message - Payload too short", addr_info);
            return false;
        }

        uint8_t reg_id = payload[0];
        uint8_t slot = payload[1];

        // ESP_LOGI(TAG, "%s Register message received - Reg=0x%02X, Slot=0x%02X, searching handlers...",
        //          addr_info, reg_id, slot);

        // Find matching handler in dispatch table
        for (int i = 0; i < REGISTER_HANDLER_COUNT; i++) {
            const register_handler_t *entry = &REGISTER_HANDLERS[i];

            if (reg_id >= entry->reg_start && reg_id <= entry->reg_end && entry->slot == slot) {
                // ESP_LOGI(TAG, "  -> Matched handler: %s", entry->name);
                return entry->handler(data, len, payload, payload_len, addr_info, ctx);
            }
        }

        // If we are here it means we have an unhandled register message - log details for debugging
        
        // Format all payload bytes as hex for debugging (e.g., "7F 02 00 81")
        int payload_hex_size = payload_len * 3 + 1;
        char *payload_hex = malloc(payload_hex_size);
        if (!payload_hex) return false;
        int pos = 0;
        for (int i = 0; i < payload_len; i++) {
            pos += snprintf(&payload_hex[pos], payload_hex_size - pos, "%02X ", payload[i]);
        }
        // Remove trailing space
        if (pos > 0 && payload_hex[pos - 1] == ' ') {
            payload_hex[pos - 1] = '\0';
        }

        ESP_LOGW(TAG, "%s Unhandled register - Reg=0x%02X, Slot=0x%02X, Payload[%d]: %s",
                 addr_info, reg_id, slot, payload_len, payload_hex);
        free(payload_hex);
        return false;
    }

    // Configuration messages
    if (match_pattern(data, len, MSG_TYPE_LIGHT_CONFIG)) {
        return handle_light_config(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_CONFIG)) {
        return handle_config(data, len, payload, payload_len, addr_info, ctx);
    }

    // Operational messages
    if (match_pattern(data, len, MSG_TYPE_MODE)) {
        return handle_mode(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_CHANNELS)) {
        return handle_channels(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_CHANNEL_STATUS)) {
        return handle_channel_status(data, len, payload, payload_len, addr_info, ctx);
    }

    // Temperature messages
    if (match_pattern(data, len, MSG_TYPE_TEMP_SETTING)) {
        return handle_temp_setting(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TEMP_READING)) {
        return handle_temp_reading(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TEMP_READING2)) {
        return handle_temp_reading2(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_HEATER)) {
        return handle_heater(data, len, payload, payload_len, addr_info, ctx);
    }

    // Chlorinator messages
    if (match_pattern(data, len, MSG_TYPE_CHLOR)) {
        // Dispatch to chlorinator sub-handlers based on sub-type
        const uint8_t *sub = &data[7];

        if (match_pattern(sub, 4, CHLOR_PH_SETPOINT)) {
            return handle_chlor_ph_setpoint(data, len, payload, payload_len, addr_info, ctx);
        }
        if (match_pattern(sub, 4, CHLOR_ORP_SETPOINT)) {
            return handle_chlor_orp_setpoint(data, len, payload, payload_len, addr_info, ctx);
        }
        if (match_pattern(sub, 4, CHLOR_PH_READING)) {
            return handle_chlor_ph_reading(data, len, payload, payload_len, addr_info, ctx);
        }
        if (match_pattern(sub, 4, CHLOR_ORP_READING)) {
            return handle_chlor_orp_reading(data, len, payload, payload_len, addr_info, ctx);
        }
    }

    // Gateway messages
    if (match_pattern(data, len, MSG_TYPE_SERIAL_NUMBER)) {
        return handle_serial_number(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_GATEWAY_IP)) {
        return handle_gateway_ip(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_GATEWAY_COMMS)) {
        return handle_gateway_comms(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_GATEWAY_VERSION)) {
        return handle_gateway_version(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_GATEWAY_STATUS)) {
        return handle_gateway_status(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_REGISTER_READ_REQUEST)) {
        return handle_register_read_request(data, len, payload, payload_len, addr_info, ctx);
    }

    // Gateway control commands
    if (match_pattern(data, len, MSG_TYPE_CHANNEL_TOGGLE_CMD)) {
        return handle_channel_toggle_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TEMP_SET_CMD)) {
        return handle_temp_set_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_LIGHT_CONTROL_CMD)) {
        return handle_light_control_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_MODE_CONTROL_CMD)) {
        return handle_mode_control_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    // Controller info messages
    if (match_pattern(data, len, MSG_TYPE_CONTROLLER_TIME)) {
        return handle_controller_time(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_VERSION)) {
        return handle_touchscreen_version(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN1)) {
        return handle_touchscreen_unknown1(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_VALVE_STATE)) {
        return handle_valve_state(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN2)) {
        return handle_touchscreen_unknown2(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN3)) {
        return handle_touchscreen_unknown3(data, len, payload, payload_len, addr_info, ctx);
    }

    // No handler matched - log as unknown
    return handle_unknown(data, len, payload, payload_len, addr_info, ctx);
}
