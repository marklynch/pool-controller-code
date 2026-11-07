#ifndef MESSAGE_DECODER_H
#define MESSAGE_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pool_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Message decoder configuration
 *
 * This structure holds the context needed for decoding pool bus messages.
 * For unit testing, enable_mqtt can be set to false to skip MQTT publishing.
 */
typedef struct {
    pool_state_t *pool_state;          // Pointer to global pool state
    SemaphoreHandle_t state_mutex;     // Mutex protecting pool state
    bool enable_mqtt;                   // If false, skip MQTT publishing (for testing)
} message_decoder_context_t;

/**
 * Decode a pool bus message
 *
 * Parses and processes messages from the pool controller bus.
 * Updates pool state and optionally publishes to MQTT.
 *
 * @param data Message bytes (must start with 0x02, end with 0x03)
 * @param len Length of message in bytes
 * @param ctx Decoder context (pool state, mutex, MQTT enable flag)
 * @return true if message was decoded, false if unknown/malformed
 */
bool decode_message(const uint8_t *data, int len, message_decoder_context_t *ctx);

/**
 * Verify checksum for protocol messages
 *
 * Checksum = sum of all data bytes from index 10 to (len-3), low byte only.
 * The checksum byte is at (len-2).
 *
 * @param data Message bytes
 * @param len Length of message
 * @return true if checksum is valid, false otherwise
 */
bool verify_message_checksum(const uint8_t *data, int len);

/**
 * Get device name from address bytes.
 *
 * If the address is known, returns a static label. Otherwise formats
 * "Unknown 0xHHLL" into the caller-supplied buffer and returns a pointer
 * to it. The buffer must be at least 16 bytes.
 */
const char* get_device_name(uint8_t addr_hi, uint8_t addr_lo, char *fallback_buf, size_t buf_size);

/**
 * Get a lower_snake_case slug for a device, suitable for MQTT topic paths.
 *
 * Derived from get_device_name() — alphanumerics pass through (lowercased),
 * everything else (spaces, slashes, etc.) collapses to a single underscore.
 * Unknown sources slug to "unknown_HHLL" (no "0x" prefix).
 *
 * Examples: "Connect 8/10" → "connect_8_10", "Genus Heater" → "genus_heater",
 *           addr 0x0072 (unknown) → "unknown_0072".
 *
 * The buffer must be at least 24 bytes to hold the longest known slug
 * ("internal_salt_cell") plus a null terminator.
 */
const char* get_device_slug(uint8_t addr_hi, uint8_t addr_lo, char *buf, size_t buf_size);

/**
 * Get gateway communications status text
 *
 * @param code Status code (big-endian value from message)
 * @return Status text string
 */
const char* get_gateway_comms_status_text(uint16_t code);

// External constant arrays. LIGHTING_COLOR_NAMES lives in lighting_colors.c;
// the channel state names and helpers are declared in pool_state.h.
extern const char *LIGHTING_COLOR_NAMES[];

// Constant counts
#define LIGHTING_COLOR_COUNT 51

/**
 * Get channel type name from type code
 * Supports all channel types including special codes (0xFD=Heater, 0xFE=Light Zone)
 * @param type_code Channel type code
 * @return Channel type name, or "Unknown" if not found
 */
const char* get_channel_type_name(channel_type_t type_code);

/**
 * Get multicolor light type name from the register 0xF0 light model index
 * Known values: MULTICOLOR_LIGHT_TYPE_NONE ("None"), MULTICOLOR_LIGHT_TYPE_SLX
 * ("SLX"), MULTICOLOR_LIGHT_TYPE_DELTA ("Delta"). Otherwise formats
 * "Unknown (0xXX)" into the caller-supplied buffer and returns it.
 * @param type Multicolor light type index from register 0xF0, slot 0x01
 * @param fallback_buf Buffer for the unknown-value fallback string
 * @param buf_size Size of fallback_buf
 * @return Multicolor light type name string
 */
const char* multicolor_light_type_name(uint8_t type, char *fallback_buf, size_t buf_size);

/**
 * Get the color codes available on a multicolor light model
 * Returns the model's subset of the shared color value space (each code is an
 * index into LIGHTING_COLOR_NAMES) — see PROTOCOL.md, Light Zone Color Control.
 * @param light_type Multicolor light type index from register 0xF0, slot 0x01
 * @param count Out: number of codes (0 if the type is unknown or None)
 * @return Pointer to the code array, or NULL if the type has no color table
 */
const uint8_t* multicolor_light_color_codes(uint8_t light_type, int *count);



// Channel category codes (registers 0xF5–0xFC, slot 0x01) — coarser than the
// per-channel type codes above; only broadcast for channels that are in use.
#define CHANNEL_CATEGORY_POOL_EQUIPMENT 0x01
#define CHANNEL_CATEGORY_LIGHT          0x02
#define CHANNEL_CATEGORY_HEATER_POWER   0x03  // Controlled Heater Power
#define CHANNEL_CATEGORY_SECONDARY_HEATER 0x05  // Secondary Heater (0x04 not observed)

// Register IDs used in CMD 0x38 (register data) and CMD 0x3A (register write).
// See PROTOCOL.md Appendix A for the full dispatch table.
typedef enum {
    // Range registers 
    REG_ID_TIMER_0                 = 0x08,  // Timer index 0    (0–15 → 0x08–0x17, slot 0x04)
    REG_ID_TIMER_15                = 0x17,  // Timer index 15 (last)

    REG_ID_ACTIVE_FAVOURITE        = 0x20,  // Currently active favourite (slot 0x03): CMD 0x2A value, 0xFF=none

    REG_ID_FAVOURITE_ENABLE_0      = 0x21,  // Fav enable index 0  (0–7 → 0x21–0x28, slot 0x03)
    REG_ID_FAVOURITE_ENABLE_7      = 0x28,  // Fav enable index 7 (last)

    REG_ID_VALVE_CONFIGURED_0      = 0x26,  // Valve 0 configured flag (0–7 → 0x26–0x2D, slot 0x02; 0x26–0x28 shares range with REG_ID_FAVOURITE_ENABLE_[5-7], slot 0x03)
    REG_ID_VALVE_CONFIGURED_7      = 0x2D,  // Valve 7 configured (last)

    REG_ID_HEATER_WATER_TEMP       = 0x2C,  // Heater's own water temperature °C (slot 0x01), answered by the heater — repeats its CMD 0x16 reading

    REG_ID_WATER_TEMP              = 0x30,  // Current water temperature °C (slot 0x01) — mirror of the controller's CMD 0x16 reading

    REG_ID_FAVOURITE_LABEL_0       = 0x31,  // Fav label index 0   (0–7 → 0x31–0x38, slot 0x03)
    REG_ID_FAVOURITE_LABEL_7       = 0x38,  // Fav label index 7 (last)

    REG_ID_SOLAR_SETPOINT          = 0x3A,  // Solar temperature setpoint in °C (slot 0x01)

    REG_ID_SEPARATE_POOL_SPA_MODES = 0x64,  // "Use separate pool and spa modes" setting (slot 0x00): 0x00=No, 0x01=Yes

    REG_ID_MODE                    = 0x65,  // Pool/Spa mode (slot 0x00): MODE_SPA/MODE_POOL, same as CMD 0x14

    REG_ID_CHANNEL_TYPE_0          = 0x6C,  // Channel 0 type  (0–7 → 0x6C–0x73, slot 0x02)
    REG_ID_CHANNEL_TYPE_7          = 0x73,  // Channel 7 type (last)

    REG_ID_CHANNEL_NAME_0          = 0x7C,  // Channel 0 name  (0–7 → 0x7C–0x83, slot 0x02)
    REG_ID_CHANNEL_NAME_7          = 0x83,  // Channel 7 name (last)

    REG_ID_CHANNEL_STATE_0         = 0x8C,  // Channel 0 state (0–7 → 0x8C–0x93, slot 0x02)
    REG_ID_CHANNEL_STATE_7         = 0x93,  // Channel 7 state (last)

    REG_ID_LIGHT_ZONE_ENABLED_0    = 0x90,  // Zone 0 enabled flag (0–7 → 0x90–0x97, slot 0x01; 0x90–0x93 shares range with REG_ID_CHANNEL_STATE_[4-7], slot 0x02)
    REG_ID_LIGHT_ZONE_ENABLED_7    = 0x97,  // Zone 7 enabled    (last)

    REG_ID_LIGHT_ZONE_MULTICOLOR_0 = 0xA0,  // Zone 0 multicolor (0–7 → 0xA0–0xA7, slot 0x01)
    REG_ID_LIGHT_ZONE_MULTICOLOR_7 = 0xA7,  // Zone 7 multicolor (last)

    REG_ID_CHANNEL_LIGHT_ZONE_0    = 0xAC,  // Channel 0 light zone (0–7 → 0xAC–0xB3, slot 0x0D): 0-based zone, 0xFF = none; 0xB0–0xB3 shares range with REG_ID_LIGHT_ZONE_NAME_[0-3], slot 0x01
    REG_ID_CHANNEL_LIGHT_ZONE_7    = 0xB3,  // Channel 7 light zone (last)

    REG_ID_LIGHT_ZONE_NAME_0       = 0xB0,  // Zone 0 name code (0–7 → 0xB0–0xB7, slot 0x01)
    REG_ID_LIGHT_ZONE_NAME_7       = 0xB7,  // Zone 7 name      (last)

    REG_ID_LIGHT_ZONE_STATE_0      = 0xC0,  // Zone 0 state (0–7 → 0xC0–0xC7, slot 0x01)
    REG_ID_LIGHT_ZONE_STATE_7      = 0xC7,  // Zone 7 state (last)

    REG_ID_LIGHT_ZONE_COLOR_0      = 0xD0,  // Zone 0 color (0–7 → 0xD0–0xD7, slot 0x01, shares range with REG_ID_VALVE_LABEL_[0-7])
    REG_ID_LIGHT_ZONE_COLOR_7      = 0xD7,  // Zone 7 color (last)

    REG_ID_VALVE_LABEL_0           = 0xD0,  // Valve 0 label (0–7 → 0xD0–0xD7, slot 0x02; shares range with REG_ID_LIGHT_ZONE_COLOR_[0-7])
    REG_ID_VALVE_LABEL_7           = 0xD7,  // Valve 7 label (last)
    REG_ID_VALVE_STATE_0           = 0xE0,  // Valve 0 state (0–7 → 0xE0–0xE7, slot 0x02; shares range with REG_ID_LIGHT_ZONE_ACTIVE_[0-7])
    REG_ID_VALVE_STATE_7           = 0xE7,  // Valve 7 state (last)

    REG_ID_GATEWAY_WIFI_SSID       = 0xD0,  // Internet Gateway WiFi SSID (slot 0x07, Touchscreen -> Gateway CMD 0x3A write, ASCII string)
    REG_ID_GATEWAY_WIFI_PASSWORD   = 0xD1,  // Internet Gateway WiFi password (slot 0x07, same as above)

    REG_ID_LIGHT_ZONE_ACTIVE_0     = 0xE0,  // Zone 0 active flag (0–7 → 0xE0–0xE7, slot 0x01)
    REG_ID_LIGHT_ZONE_ACTIVE_7     = 0xE7,  // Zone 7 active (last)

    REG_ID_TIMER_CONFIGURED_0      = 0xE8,  // Timer 0 configured flag (0–15 → 0xE8–0xF7, slot 0x03; 0xE8–0xEB shares range with the heater registers, slot 0x00)
    REG_ID_TIMER_CONFIGURED_15     = 0xF7,  // Timer 15 configured (last)

    // Point registers
    REG_ID_HEATER1_TYPE          = 0xC8,  // First heater type (slot 0x00): HEATER1_TYPE_*
    REG_ID_HEATER1_POWER_CHANNEL = 0xCA,  // First heater's power channel (slot 0x00): {0x00, 0-based channel}
    REG_ID_HEATER2_CONFIGURED    = 0xCC,  // Second heater configured flag (slot 0x00)
    REG_ID_HEATER2_POWER_CHANNEL = 0xCD,  // Second heater's power channel (slot 0x00): {0x00, 0-based channel or 0xFF, 0x00}
    REG_ID_HEATER1_ONOFF        = 0xE6,  // Heater 1 on/off state
    REG_ID_HEATER1_POOL_SETPOINT = 0xE7,  // Pool temperature setpoint (Heater 1)
    REG_ID_HEATER1_SPA_SETPOINT  = 0xE8,  // Spa temperature setpoint (Heater 1)
    REG_ID_HEATER2_ONOFF         = 0xE9,  // Heater 2 on/off state — tentative, see PROTOCOL.md Appendix A
    REG_ID_HEATER2_POOL_SETPOINT = 0xEA,  // Heater 2 pool setpoint
    REG_ID_HEATER2_SPA_SETPOINT  = 0xEB,  // Heater 2 spa setpoint — tentative, see PROTOCOL.md Appendix A
    REG_ID_CHLOR_LOWEST_PH         = 0xE2,  // Lowest pH measured × 10 (slot 0x0B, Viron Chlorinator), 2-byte LE
    REG_ID_CHLOR_HIGHEST_PH        = 0xE3,  // Highest pH measured × 10 (slot 0x0B, Viron Chlorinator), 2-byte LE
    REG_ID_CHLOR_LOWEST_ORP        = 0xE4,  // Lowest ORP measured in mV (slot 0x0B, Viron Chlorinator), 2-byte LE
    REG_ID_CHLOR_HIGHEST_ORP       = 0xE5,  // Highest ORP measured in mV (slot 0x0B, Viron Chlorinator), 2-byte LE
    REG_ID_CHLOR_UNKNOWN_COUNTDOWN = 0xE9,  // Unknown countdown (slot 0x0B, Viron Chlorinator) — tentative, see PROTOCOL.md Appendix A
    REG_ID_CHLOR_UNKNOWN_VALUE     = 0xEA,  // Unknown static value (slot 0x0B, Viron Chlorinator) — tentative, see PROTOCOL.md Appendix A
    REG_ID_MULTICOLOR_LIGHT_TYPE = 0xF0,  // System-wide multicolor light type selection (slot 0x01): MULTICOLOR_LIGHT_TYPE_*
    REG_ID_CHANNEL_COUNT         = 0xF4,  // Total number of channels

    REG_ID_CHANNEL_CATEGORY_0    = 0xF5,  // Channel 0 category (0–7 → 0xF5–0xFC, slot 0x01)
    REG_ID_CHANNEL_CATEGORY_7    = 0xFC,  // Channel 7 category (last)
} reg_id_t;

#endif // MESSAGE_DECODER_H
