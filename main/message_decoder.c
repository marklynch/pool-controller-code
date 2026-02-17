#include "message_decoder.h"
#include "config.h"
#include "mqtt_publish.h"
#include "esp_log.h"
#include <string.h>

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

// 62 Temperature sensor / Unknown subsystem
static const char *MSG_TYPE_TEMP_READING =          "02 00 62 FF FF 80 00 16 0E 06";
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
static const char *MSG_TYPE_REGISTER_READ_REQUEST =   "02 00 F0 FF FF 80 00 39 0E B7";

// F0 Gateway Control Commands (Gateway -> Controller)
static const char *MSG_TYPE_CHANNEL_TOGGLE_CMD =      "02 00 F0 FF FF 80 00 10 0D 8D";
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
static bool handle_channel_type(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_name(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_color(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_active(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_label(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_register_label_generic(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);

/**
 * Register message dispatch table
 * Entries are checked in order, first match wins
 * Only includes register ranges with confirmed behavior
 */
static const register_handler_t REGISTER_HANDLERS[] = {
    // Channel configuration
    {0x6C, 0x73, 0x02, handle_channel_type,       "Channel Type"},
    {0x7C, 0x83, 0x02, handle_channel_name,       "Channel Name"},

    // Lighting zones
    {0xC0, 0xC7, 0x01, handle_light_zone_state,   "Light Zone State"},
    {0xD0, 0xD7, 0x01, handle_light_zone_color,   "Light Zone Color"},
    {0xE0, 0xE7, 0x01, handle_light_zone_active,  "Light Zone Active"},

    // Labels (slot 0x03) - only specific ranges we've observed
    {0x31, 0x38, 0x03, handle_register_label_generic, "Favourite Label"},
    {0xD0, 0xD1, 0x03, handle_valve_label,        "Valve Label"},
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
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->current_temp = current_temp;
        ctx->pool_state->temp_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

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
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->heater_on = (heater_state != 0);
        ctx->pool_state->heater_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (ctx->enable_mqtt) {
        mqtt_publish_heater(&snapshot);
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
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->spa_setpoint = spa_set_temp_c;
        ctx->pool_state->pool_setpoint = pool_set_temp_c;
        ctx->pool_state->spa_setpoint_f = spa_set_temp_f;
        ctx->pool_state->pool_setpoint_f = pool_set_temp_f;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

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
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->mode = mode;
        ctx->pool_state->mode_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

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
    char hex_str[3 * len + 1];
    int pos = 0;
    for (int i = 0; i < len; i++) {
        pos += snprintf(&hex_str[pos], sizeof(hex_str) - pos, "%02X ", data[i]);
    }
    hex_str[pos] = '\0';

    ESP_LOGW(TAG, "Unhandled: %s", hex_str);

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
    if (payload_len < 2) return false;

    uint8_t data_byte1 = payload[0];
    uint8_t data_byte2 = payload[1];

    if (data_byte1 != 0x00 || data_byte2 != 0x00) {
        ESP_LOGW(TAG, "%s Controller status - UNEXPECTED VALUE: Byte1: 0x%02X (%d), Byte2: 0x%02X (%d) (expected 0x00 0x00)",
                 addr_info, data_byte1, data_byte1, data_byte2, data_byte2);
    } else {
        ESP_LOGI(TAG, "%s Controller status - Byte1: 0x%02X (%d), Byte2: 0x%02X (%d)",
                 addr_info, data_byte1, data_byte1, data_byte2, data_byte2);
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
    ESP_LOGI(TAG, "%s Serial number - %lu (0x%08lX)", addr_info, (unsigned long)serial, (unsigned long)serial);

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
    ESP_LOGI(TAG, "%s Register read request - Reg=0x%02X, Slot=0x%02X", addr_info, reg_id, slot_id);

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
        if (channel_idx < 8 && ctx->pool_state->channels[channel_idx].configured) {
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

    // Calculate zone number from register ID (0xC0 = Zone 1, 0xC1 = Zone 2, etc.)
    if (reg_id >= 0xC0 && reg_id <= 0xC7 && slot == 0x01) {
        uint8_t zone_num = reg_id - 0xC0 + 1;
        const char *state_name = (state == 0x00) ? "Off" : (state == 0x01) ? "Auto" : (state == 0x02) ? "On" : "Unknown";

        ESP_LOGI(TAG, "%s Gateway light control command - Zone %d -> %s (0x%02X)",
                 addr_info, zone_num, state_name, state);
    } else {
        ESP_LOGW(TAG, "%s Gateway light control command - Unknown Reg=0x%02X, Slot=0x%02X, State=0x%02X",
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
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->ph_setpoint = value;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

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
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->orp_setpoint = value;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

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
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->ph_reading = value;
        ctx->pool_state->ph_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

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
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->orp_reading = value;
        ctx->pool_state->orp_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

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

            // Only log and publish if this is the first time
            if (!was_configured) {
                ESP_LOGI(TAG, "%s Lighting zone %d configured", addr_info, zone_idx + 1);
                should_publish = true;
            }

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

// ======================================================
// Register message handlers (dispatched by register range and slot)
// ======================================================

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
 * Register range: 0xD0-0xD1, Slot: 0x03
 */
static bool handle_valve_label(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    const char *label = (const char *)&payload[2];
    uint8_t zone_num = reg_id - 0xD0 + 1;

    ESP_LOGI(TAG, "%s Valve zone %d label (0x%02X) - \"%s\"", addr_info, zone_num, reg_id, label);

    // Update pool state
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        int slot = -1;
        for (int i = 0; i < 32; i++) {
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
    const char *label = (const char *)&payload[2];

    ESP_LOGI(TAG, "%s Register 0x%02X label - \"%s\"", addr_info, reg_id, label);

    // Update pool state
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        int slot = -1;
        for (int i = 0; i < 32; i++) {
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
            uint8_t state = payload[payload_idx + 1];
            const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";

            if (ch_type == CHANNEL_UNUSED) {
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                ctx->pool_state->channels[ch_num - 1].configured = false;
            } else {
                const char *type_name = get_channel_type_name(ch_type);
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

    // Must start with 0x02 and end with 0x03
    if (len < 7 || data[0] != 0x02 || data[len - 1] != 0x03) {
        return false;
    }

    // Log full message before decoding
    char full_msg[3 * len + 1];
    int msg_pos = 0;
    for (int i = 0; i < len && msg_pos < (int)sizeof(full_msg) - 4; i++) {
        msg_pos += snprintf(&full_msg[msg_pos], sizeof(full_msg) - msg_pos, "%02X ", data[i]);
    }
    full_msg[msg_pos] = '\0';
    ESP_LOGI(TAG, "RX MSG: %s", full_msg);

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

    // Dispatch to message handlers

    // Register messages (NEW dispatch table approach)
    if (len >= 13 && match_pattern(data, len, MSG_TYPE_REGISTER)) {
        // Verify checksum relationship: byte[8] + 8 should equal byte[9]
        uint8_t cmd = data[8];
        uint8_t sub = data[9];

        if ((cmd + 8) != sub) {
            ESP_LOGW(TAG, "%s Register message - Invalid CMD/SUB relationship: "
                     "0x%02X + 8 != 0x%02X", addr_info, cmd, sub);
            return false;
        }

        // Extract register ID and slot
        if (payload_len < 2) {
            ESP_LOGW(TAG, "%s Register message - Payload too short", addr_info);
            return false;
        }

        uint8_t reg_id = payload[0];
        uint8_t slot = payload[1];

        ESP_LOGI(TAG, "%s Register message received - Reg=0x%02X, Slot=0x%02X, searching handlers...",
                 addr_info, reg_id, slot);

        // Find matching handler in dispatch table
        for (int i = 0; i < REGISTER_HANDLER_COUNT; i++) {
            const register_handler_t *entry = &REGISTER_HANDLERS[i];

            if (reg_id >= entry->reg_start && reg_id <= entry->reg_end && entry->slot == slot) {
                // ESP_LOGI(TAG, "  -> Matched handler: %s", entry->name);
                return entry->handler(data, len, payload, payload_len, addr_info, ctx);
            }
        }

        // No handler found - log as unhandled register with full payload dump
        ESP_LOGI(TAG, "  -> No handler matched, logging as unhandled");

        // Format all payload bytes as hex for debugging (e.g., "7F 02 00 81")
        char payload_hex[payload_len * 3 + 1];
        int pos = 0;
        for (int i = 0; i < payload_len; i++) {
            pos += snprintf(&payload_hex[pos], sizeof(payload_hex) - pos, "%02X ", payload[i]);
        }
        // Remove trailing space
        if (pos > 0 && payload_hex[pos - 1] == ' ') {
            payload_hex[pos - 1] = '\0';
        }

        ESP_LOGW(TAG, "%s Unhandled register - Reg=0x%02X, Slot=0x%02X, Payload[%d]: %s",
                 addr_info, reg_id, slot, payload_len, payload_hex);
        return false;
    }

    // Configuration messages
    if (len >= 14 && match_pattern(data, len, MSG_TYPE_LIGHT_CONFIG)) {
        return handle_light_config(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 12 && match_pattern(data, len, MSG_TYPE_CONFIG)) {
        return handle_config(data, len, payload, payload_len, addr_info, ctx);
    }

    // Operational messages
    if (len >= 13 && match_pattern(data, len, MSG_TYPE_MODE)) {
        return handle_mode(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 13 && match_pattern(data, len, MSG_TYPE_CHANNELS)) {
        return handle_channels(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 13 && match_pattern(data, len, MSG_TYPE_CHANNEL_STATUS)) {
        return handle_channel_status(data, len, payload, payload_len, addr_info, ctx);
    }

    // Temperature messages
    if (len >= 14 && match_pattern(data, len, MSG_TYPE_TEMP_SETTING)) {
        return handle_temp_setting(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 12 && match_pattern(data, len, MSG_TYPE_TEMP_READING)) {
        return handle_temp_reading(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 12 && match_pattern(data, len, MSG_TYPE_HEATER)) {
        return handle_heater(data, len, payload, payload_len, addr_info, ctx);
    }

    // Chlorinator messages
    if (len >= 13 && match_pattern(data, len, MSG_TYPE_CHLOR)) {
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
    if (len >= 15 && match_pattern(data, len, MSG_TYPE_SERIAL_NUMBER)) {
        return handle_serial_number(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 19 && match_pattern(data, len, MSG_TYPE_GATEWAY_IP)) {
        return handle_gateway_ip(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 14 && match_pattern(data, len, MSG_TYPE_GATEWAY_COMMS)) {
        return handle_gateway_comms(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 12 && match_pattern(data, len, MSG_TYPE_REGISTER_READ_REQUEST)) {
        return handle_register_read_request(data, len, payload, payload_len, addr_info, ctx);
    }

    // Gateway control commands
    if (len >= 13 && match_pattern(data, len, MSG_TYPE_CHANNEL_TOGGLE_CMD)) {
        return handle_channel_toggle_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 13 && match_pattern(data, len, MSG_TYPE_LIGHT_CONTROL_CMD)) {
        return handle_light_control_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 13 && match_pattern(data, len, MSG_TYPE_MODE_CONTROL_CMD)) {
        return handle_mode_control_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    // Controller info messages
    if (len >= 14 && match_pattern(data, len, MSG_TYPE_CONTROLLER_TIME)) {
        return handle_controller_time(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 13 && match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_VERSION)) {
        return handle_touchscreen_version(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 14 && match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN1)) {
        return handle_touchscreen_unknown1(data, len, payload, payload_len, addr_info, ctx);
    }

    if (len >= 13 && match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN2)) {
        return handle_touchscreen_unknown2(data, len, payload, payload_len, addr_info, ctx);
    }

    // No handler matched - log as unknown
    return handle_unknown(data, len, payload, payload_len, addr_info, ctx);
}
