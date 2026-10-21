#include "message_decoder.h"
#include "config.h"
#include "framing.h"
#include "mqtt_publish.h"
#include "register_requester.h"
#include "unknown_buffer.h"
#include "filter_pump_type.h"
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
// Undocumented-payload recording
// ======================================================

// Record a frame that a handler recognised but whose payload carries an
// undocumented field value (e.g. an unexpected state byte). The handler should
// still apply and publish the fields it *does* understand, then call this and
// return true — the frame is counted as decoded, and surfaced on the Unknown
// Messages page (as a non-error "undocumented" entry) for protocol research.
//
// Call this *outside* any ctx->state_mutex critical section: it takes the
// unknown buffer's own mutex, so recording from inside the state lock would
// hold state_mutex while blocking on a second lock (extra contention / longer
// hold time). No code currently acquires state_mutex while holding the unknown
// buffer mutex, so there is no deadlock cycle today — but keeping the two locks
// disjoint also future-proofs against one being introduced. Invariant is
// convention-only; it is not enforced here.
static inline void record_undocumented(const uint8_t *data, int len)
{
    unknown_buffer_record(data, len, UNKNOWN_REASON_UNDOCUMENTED_PAYLOAD);
}

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

// Device status (CMD 0x12) is dispatched on the CMD byte in dispatch_message()
// and routed by source address in handle_device_status() — the payload
// layout differs per device. See PROTOCOL.md command `0x12`.

// Register data (CMD 0x38) is dispatched source-agnostically in
// dispatch_message() — see PROTOCOL.md command `0x38`. Routing is by
// (reg_id, slot) via REGISTER_HANDLERS, independent of the source device.

// Message type patterns (messages start with 0x02, end with 0x03)
// 50 Main Controller (Connect 10)
static const char *MSG_TYPE_TEMP_SETTING =          "02 00 50 FF FF 80 00 17 10 F7";
static const char *MSG_TYPE_CONFIG =                "02 00 50 FF FF 80 00 26 0E 04";
static const char *MSG_TYPE_MODE =                  "02 00 50 FF FF 80 00 14 0D F1";
static const char *MSG_TYPE_CHANNELS =              "02 00 50 00 6F 80 00 0D 0D 5B";
static const char *MSG_TYPE_CHANNEL_STATUS =        "02 00 50 FF FF 80 00 0B 25 00";
static const char *MSG_TYPE_LIGHT_CONFIG =          "02 00 50 FF FF 80 00 06 0E E4";
static const char *MSG_TYPE_CONTROLLER_TIME =       "02 00 50 FF FF 80 00 FD 0F DC";
static const char *MSG_TYPE_TOUCHSCREEN_UNKNOWN3 =  "02 00 50 FF FF 80 00 05 0D E2";

// Motorised valve actuators (Touchscreen 0x0050 -> Internal Control 0x007F),
// emitted as a pair on every mode change. See PROTOCOL.md commands `0x1A` and
// `0x41`.
static const char *MSG_TYPE_PRE_VALVE_FRAME =       "02 00 50 00 7F 00 00 1A 0B F6";
static const char *MSG_TYPE_VALVE_ACTUATOR_CMD =    "02 00 50 00 7F 80 00 41 0E A0";

// Water temperature reading (CMD 0x16) is dispatched source-agnostically in
// dispatch_message() — see PROTOCOL.md command `0x16`. Observed from the
// Connect 8/10 Controller (0x0062, LEN 0x0E, 2-byte payload temp1+temp2),
// and the heater devices — Genus 0x0070, Viron 0x0071, HiNRG 0x0072 and
// ICI 0x0074 (LEN 0x0D, 1-byte payload temp1 only).

// 62 Connect 8/10 Controller
static const char *MSG_TYPE_CONTROLLER_UNKNOWN2B =  "02 00 62 00 50 80 00 2B 0E 6D";

// 70 Genus Heater (Active i25 Evo)
static const char *MSG_TYPE_GENUS_HEATER_TEMP_SETTING = "02 00 70 FF FF 80 00 17 0E 15";

// 71 AstralPool Viron Gas Heater
// CMD 0x16 (temperature reading) is handled by the source-agnostic handler above.
static const char *MSG_TYPE_VIRON_HEATER_TEMP_SETTING = "02 00 71 FF FF 80 00 17 0E 16";

// 72 AstralPool HiNRG Gas Heater
// CMD 0x16 (temperature reading) is handled by the source-agnostic handler above.
static const char *MSG_TYPE_HINRG_HEATER_TEMP_SETTING = "02 00 72 FF FF 80 00 17 0E 17";

// 74 AstralPool ICI Gas Heater (Astral/Fluidra ICI 400B NG)
// CMD 0x16 (temperature reading) is handled by the source-agnostic handler above.
static const char *MSG_TYPE_ICI_HEATER_TEMP_SETTING = "02 00 74 FF FF 80 00 17 0E 19";

// Chlorinator setpoints (CMD 0x1D) and readings (CMD 0x1F) are dispatched
// source-agnostically in dispatch_message() — see PROTOCOL.md commands `0x1D`
// and `0x1F`. Same payload shape from both 0x0090 RolaChem and 0x0084 Viron.

// Set channel state (CMD 0x0F) is dispatched source-agnostically in
// dispatch_message() — see PROTOCOL.md command `0x0F`. The Touchscreen does
// not validate the source address at all: 0x0084 (Viron) and 0x0081 (VX 11S
// v3) are confirmed chlorinator sources on the bus, the Connect 8/10 (0x0062)
// sends it for its Cleaning button, and this firmware originates it
// unconditionally as its own 0xAC1D.

// Internet Gateway info (CMD 0x37) is dispatched on the CMD byte in
// dispatch_message() and routed by its item ID (byte 10) in
// handle_gateway_info() — see PROTOCOL.md command `0x37`. Variants of
// different items can share a LENGTH (comms status and firmware version are
// both 0x0F), so the LENGTH byte can't tell them apart.

// Register read request (CMD 0x39) is dispatched source-agnostically in
// dispatch_message() — see PROTOCOL.md command `0x39`. Observed from the
// Internet Gateway (0x00F0) and the Genus Heater (0x0070); payload shape
// `{reg_id, slot_id}` is identical.

// Temperature setpoint command (CMD 0x19) is dispatched source-agnostically
// in dispatch_message() — see PROTOCOL.md command `0x19`. Slot byte (payload[0])
// selects Pool/Spa (0x01/0x02, 3-byte payload from 0x00F0 Gateway) or heater
// pair (0x03, 5-byte payload from 0x0050 Touch Screen to 0x007F).

// A0 Viron Pump Telemetry
static const char *MSG_TYPE_PUMP_SPEED =              "02 00 A0 FF FF 80 00 3B 0E 69";
static const char *MSG_TYPE_PUMP_SPEED_V2 =           "02 00 A0 FF FF 80 00 3B 10 6B";
static const char *MSG_TYPE_PUMP_BUTTONS =            "02 00 A0 FF FF 80 00 1B 0D 48";

// ======================================================
// Lookup tables and constants
// ======================================================

// Channel type lookup table
typedef struct {
    channel_type_t code;
    const char *name;
} channel_type_entry_t;

static const channel_type_entry_t CHANNEL_TYPE_TABLE[] = {
    {CHANNEL_TYPE_UNUSED,           "Unused"},
    {CHANNEL_TYPE_FILTER,           "Filter"},
    {CHANNEL_TYPE_CLEANING,         "Cleaning"},
    {CHANNEL_TYPE_HEATER_PUMP,      "Heater Pump"},
    {CHANNEL_TYPE_BOOSTER,          "Booster"},
    {CHANNEL_TYPE_WATERFALL,        "Waterfall"},
    {CHANNEL_TYPE_FOUNTAIN,         "Fountain"},
    {CHANNEL_TYPE_SPA_PUMP,         "Spa Pump"},
    {CHANNEL_TYPE_SOLAR,            "Solar"},
    {CHANNEL_TYPE_BLOWER,           "Blower"},
    {CHANNEL_TYPE_SWIMJET,          "Swimjet"},
    {CHANNEL_TYPE_JETS,             "Jets"},
    {CHANNEL_TYPE_SPA_JETS,         "Spa Jets"},
    {CHANNEL_TYPE_OVERFLOW,         "Overflow"},
    {CHANNEL_TYPE_SPILLWAY,         "Spillway"},
    {CHANNEL_TYPE_AUDIO,            "Audio"},
    {CHANNEL_TYPE_HOT_SEAT,         "Hot Seat"},
    {CHANNEL_TYPE_HEATER_POWER,     "Heater Power"},
    {CHANNEL_TYPE_CUSTOM_NAME,      "Custom Name"},
    {CHANNEL_TYPE_SECONDARY_HEATER, "Secondary Heater"},
    {CHANNEL_TYPE_SOLAR_CONTROLLER, "Solar Controller"},
    {CHANNEL_TYPE_HEATER,           "Heater"},
    {CHANNEL_TYPE_LIGHT_ZONE,       "Light Zone"},
};

#define CHANNEL_TYPE_TABLE_SIZE (sizeof(CHANNEL_TYPE_TABLE) / sizeof(CHANNEL_TYPE_TABLE[0]))

// Gas Heater Status Field Bitmasks
static const int GAS_HEATER_BITMASK_HEATER_ON =                 0x01;
static const int GAS_HEATER_BITMASK_WATER_FLOW =                0x02;
static const int GAS_HEATER_BITMASK_GAS_VALVE =                 0x04;
static const int GAS_HEATER_BITMASK_BURNER_ALIGHT =             0x08;
static const int GAS_HEATER_BITMASK_LOCKED_OUT =                0x10;
static const int GAS_HEATER_BITMASK_GENERAL_SERVICE_REQUIRED =  0x20;
static const int GAS_HEATER_BITMASK_IGNITION_SERVICE_REQUIRED = 0x40;
static const int GAS_HEATER_BITMASK_COOLING_AVAILABLE =         0x80;
// The lower 5 bits are for general heater functions
static const int GAS_HEATER_BITMASK_FUNCTIONAL_STATUS =         0x1F;

/**
 * Get channel type name from type code
 * @param type_code Channel type code (0x00-0x12, 0xFD, 0xFE)
 * @return Channel type name, or "Unknown" if not found
 */
const char* get_channel_type_name(channel_type_t type_code) {
    for (int i = 0; i < CHANNEL_TYPE_TABLE_SIZE; i++) {
        if (CHANNEL_TYPE_TABLE[i].code == type_code) {
            return CHANNEL_TYPE_TABLE[i].name;
        }
    }
    return "Unknown";
}

// True if type_code is one of the documented channel types (see PROTOCOL.md
// 0x0B "Channel Types"). Used to flag undocumented type codes for research.
static bool channel_type_is_known(channel_type_t type_code) {
    for (int i = 0; i < CHANNEL_TYPE_TABLE_SIZE; i++) {
        if (CHANNEL_TYPE_TABLE[i].code == type_code) {
            return true;
        }
    }
    return false;
}

// Command byte name lookup table — used to annotate unhandled messages in logs.
// Some CMD bytes are source-dependent (e.g. 0x12 from 0x0050 vs 0x00F0 vs
// 0x0062); the labels here are generic. See PROTOCOL.md for the full
// per-source semantics.
typedef struct {
    uint8_t cmd;
    const char *name;
} cmd_name_entry_t;

static const cmd_name_entry_t CMD_NAME_TABLE[] = {
    {0x05, "Touchscreen Unknown 3"},
    {0x06, "Lighting Zone Config"},
    {0x07, "Lighting Zone Color"},
    {0x0A, "Firmware Version"},
    {0x0B, "Channel Status"},
    {0x0D, "Active Channels Bitmask"},
    {0x0F, "Set Channel State"},
    {0x10, "Channel Toggle Cmd"},
    {0x12, "Device Status"},
    {0x14, "Mode"},
    {0x15, "Mode Set Cmd"},
    {0x16, "Temperature Reading"},
    {0x17, "Temperature Setting"},
    {0x18, "Pump Speed Command"},
    {0x19, "Temp Set Cmd"},
    {0x1D, "Chlorinator Setpoint"},
    {0x1F, "Chlorinator Reading"},
    {0x25, "Valve Sync"},
    {0x26, "Configuration"},
    {0x27, "Valve State"},
    {0x29, "Valve Button"},
    {0x2A, "Favourite Cmd"},
    {0x2B, "Controller Heartbeat"},
    {0x2C, "Solar Status"},
    {0x2D, "Solar Setpoint"},
    {0x31, "Temperature Reading (alt)"},
    {0x36, "Gateway Info Request"},
    {0x37, "Gateway Info"},
    {0x38, "Register Response"},
    {0x39, "Register Request"},
    {0x3A, "Light Zone Control Cmd"},
    {0x3C, "Light Resync Cmd"},
    {0xFD, "Controller Day/Time"},
};

#define CMD_NAME_TABLE_SIZE (sizeof(CMD_NAME_TABLE) / sizeof(CMD_NAME_TABLE[0]))

/**
 * Get a generic name for a CMD byte.
 *
 * If the CMD is in the table, returns the static table label.
 * Otherwise formats "Unknown CMD 0xXX" into the caller-supplied buffer and
 * returns a pointer to it. The buffer must be at least 20 bytes.
 *
 * Labels are intentionally generic since many CMDs are source-dependent.
 */
static const char* get_cmd_name(uint8_t cmd, char *fallback_buf, size_t buf_size) {
    for (int i = 0; i < CMD_NAME_TABLE_SIZE; i++) {
        if (CMD_NAME_TABLE[i].cmd == cmd) {
            return CMD_NAME_TABLE[i].name;
        }
    }
    snprintf(fallback_buf, buf_size, "Unknown CMD 0x%02X", cmd);
    return fallback_buf;
}

const char* multicolor_light_type_name(uint8_t type, char *fallback_buf, size_t buf_size) {
    switch (type) {
        case MULTICOLOR_LIGHT_TYPE_NONE:  return "None";
        case MULTICOLOR_LIGHT_TYPE_SLX:   return "SLX";
        case MULTICOLOR_LIGHT_TYPE_DELTA: return "Delta";
        default:
            snprintf(fallback_buf, buf_size, "Unknown (0x%02X)", type);
            return fallback_buf;
    }
}

// Per-model color code subsets of the shared color value space (indexes into
// LIGHTING_COLOR_NAMES) — see PROTOCOL.md, Light Zone Color Control.
static const uint8_t SLX_COLOR_CODES[] = {
    0x01, 0x02, 0x04, 0x05, 0x07, 0x08, 0x0A, 0x0D, 0x0E, 0x0F, 0x10, 0x11
};
static const uint8_t DELTA_COLOR_CODES[] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C
};

const uint8_t* multicolor_light_color_codes(uint8_t light_type, int *count) {
    switch (light_type) {
        case MULTICOLOR_LIGHT_TYPE_SLX:
            *count = sizeof(SLX_COLOR_CODES);
            return SLX_COLOR_CODES;
        case MULTICOLOR_LIGHT_TYPE_DELTA:
            *count = sizeof(DELTA_COLOR_CODES);
            return DELTA_COLOR_CODES;
        default:
            *count = 0;
            return NULL;
    }
}

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

// Gas heater status names (indexed by gas_heater_status_t)
const char *HEATER_STATUS_NAMES[] = {
    "Off",              // HEATER_OFF
    "No Flow",          // HEATER_ON_NO_FLOW
    "Igniting",         // HEATER_IGNITING
    "Heating",          // HEATER_HEATING
    "Setpoint Reached", // HEATER_SETPOINT_REACHED
    "Cooldown",         // HEATER_COOLDOWN
    "Locked Out",       // HEATER_LOCKED_OUT
};

// Gas heater burner state names (indexed by gas_heater_burner_state_t)
const char *BURNER_STATE_NAMES[] = {
    "Off",      // BURNER_OFF
    "Igniting", // BURNER_IGNITING
    "Alight",   // BURNER_ALIGHT
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

// Lighting color names are defined in lighting_colors.c (shared color value
// space; see PROTOCOL.md, Light Zone Color Control)

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

const char* get_device_name(uint8_t addr_hi, uint8_t addr_lo, char *fallback_buf, size_t buf_size)
{
    if (addr_hi == 0xFF && addr_lo == 0xFF) return "Broadcast";
    if (addr_hi == SELF_DEVICE_ID_HI && addr_lo == SELF_DEVICE_ID_LO) return "Pool Controller ESP32";
    if (addr_hi == 0x00) {
        switch (addr_lo) {
            // Low nibble is the model within a family (upper 12 bits) — see
            // PROTOCOL.md, Device Addresses. Names for addresses not yet seen
            // on a bus come from the Touchscreen's own device name table.
            case 0x40: return "Delta Lighting";
            case 0x41: return "Connect Lite";
            case 0x50: return "Touch Screen";
            case 0x60: return "Relay Board";
            case 0x61: return "Plus 4 Relay Board";
            case 0x62: return "Connect 8/10";
            case 0x63: return "Connect Lite Plus";
            case 0x6F: return "Internal Channels";
            case 0x7F: return "Internal Control";
            case 0x70: return "Genus Heater";
            case 0x71: return "Viron Heater";
            case 0x72: return "HiNRG Gas Heater";
            case 0x74: return "ICI Gas Heater";
            case 0x80: return "VX Chlorinator TM";
            case 0x81: return "VX 11S v3 Salt Chlorinator";
            case 0x82: return "E-Series Chlorinator";
            case 0x83: return "US VX Chlorinator";
            case 0x84: return "Viron Chlorinator";
            case 0x90: return "RolaChem";
            case 0xA0: return "Viron XT Pump";
            case 0xA1: return "Solar Pump";
            case 0xA2: return "Viron Three-speed Pump";
            case 0xB0: return "Genus II FM Receiver";
            case 0xB1: return "Wireless Transceiver";
            case 0xB2: return "RF Handheld Remote";
            case 0xC0: return "Spa Remote TM";
            case 0xC1: return "Spa Remote NT";
            case 0xC2: return "Plus 4 Controller";
            case 0xD0: return "Solar Controller";
            case 0xD1: return "Solar Roof Sensor";
            case 0xF0: return "Internet Gateway";
        }
    }
    snprintf(fallback_buf, buf_size, "Unknown 0x%02X%02X", addr_hi, addr_lo);
    return fallback_buf;
}

const char* get_device_slug(uint8_t addr_hi, uint8_t addr_lo, char *buf, size_t buf_size)
{
    if (buf_size == 0) return buf;

    if (addr_hi == 0xFF && addr_lo == 0xFF) {
        snprintf(buf, buf_size, "broadcast");
        return buf;
    }

    char name_buf[16];
    const char *name = get_device_name(addr_hi, addr_lo, name_buf, sizeof(name_buf));

    // Unknown sources: skip the "0x" prefix that would otherwise leak into the slug.
    if (strncmp(name, "Unknown ", 8) == 0) {
        snprintf(buf, buf_size, "unknown_%02x%02x", addr_hi, addr_lo);
        return buf;
    }

    // Slugify: lowercase alphanumerics pass through; everything else collapses to a single underscore.
    size_t pos = 0;
    bool last_was_underscore = true;  // start true to suppress a leading underscore
    for (const char *p = name; *p && pos + 1 < buf_size; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            buf[pos++] = c - 'A' + 'a';
            last_was_underscore = false;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            buf[pos++] = c;
            last_was_underscore = false;
        } else if (!last_was_underscore) {
            buf[pos++] = '_';
            last_was_underscore = true;
        }
    }
    if (pos > 0 && buf[pos - 1] == '_') pos--;  // strip trailing underscore
    buf[pos] = '\0';
    return buf;
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
static bool handle_timer_configured(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_type(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_name(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_enabled(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_color(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_active(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_multicolor(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_name(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_label(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_configured(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_state_register(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_favourite_label(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_favourite_enable(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_active_favourite(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_temp_setpoint(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_solar_setpoint(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_water_temp_register(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_multicolor_light_type(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_count(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_category(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_touchscreen_unknown3(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_heater1_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_heater2_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_heater2_configured(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_heater1_type(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_heater_power_channel(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_chlor_unknown_countdown(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_chlor_unknown_value(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool apply_solar_status(pool_state_t *ps, channel_state_t *ch);

/**
 * Register message dispatch table
 * Entries are checked in order, first match wins
 * Only includes register ranges with confirmed behavior
 */
static const register_handler_t REGISTER_HANDLERS[] = {
    // Timers (slot 0x04, registers 0x08-0x17 = timers 1-16)
    {REG_ID_TIMER_0,                 REG_ID_TIMER_15,                0x04, handle_timer,                 "Timer"},

    // Timer configured flags (slot 0x03, registers 0xE8-0xF7 = timers 1-16)
    {REG_ID_TIMER_CONFIGURED_0,      REG_ID_TIMER_CONFIGURED_15,     0x03, handle_timer_configured,      "Timer Configured"},

    // Channel configuration
    {REG_ID_CHANNEL_TYPE_0,          REG_ID_CHANNEL_TYPE_7,          0x02, handle_channel_type,          "Channel Type"},
    {REG_ID_CHANNEL_NAME_0,          REG_ID_CHANNEL_NAME_7,          0x02, handle_channel_name,          "Channel Name"},
    {REG_ID_CHANNEL_STATE_0,         REG_ID_CHANNEL_STATE_7,         0x02, handle_channel_state,         "Channel State"},

    // Lighting zones (slot 0x01, 8 zones per family)
    {REG_ID_LIGHT_ZONE_ENABLED_0,    REG_ID_LIGHT_ZONE_ENABLED_7,    0x01, handle_light_zone_enabled,    "Light Zone Enabled"},
    {REG_ID_LIGHT_ZONE_MULTICOLOR_0, REG_ID_LIGHT_ZONE_MULTICOLOR_7, 0x01, handle_light_zone_multicolor, "Light Zone Multicolor"},
    {REG_ID_LIGHT_ZONE_NAME_0,       REG_ID_LIGHT_ZONE_NAME_7,       0x01, handle_light_zone_name,       "Light Zone Name"},
    {REG_ID_LIGHT_ZONE_STATE_0,      REG_ID_LIGHT_ZONE_STATE_7,      0x01, handle_light_zone_state,      "Light Zone State"},
    {REG_ID_LIGHT_ZONE_COLOR_0,      REG_ID_LIGHT_ZONE_COLOR_7,      0x01, handle_light_zone_color,      "Light Zone Color"},
    {REG_ID_LIGHT_ZONE_ACTIVE_0,     REG_ID_LIGHT_ZONE_ACTIVE_7,     0x01, handle_light_zone_active,     "Light Zone Active"},

    // Valve labels (slot 0x02, registers 0xD0-0xD7 = valves 1-8)
    {REG_ID_VALVE_LABEL_0,           REG_ID_VALVE_LABEL_7,           0x02, handle_valve_label,           "Valve Label"},

    // Valve configured flags (slot 0x02, registers 0x26-0x2D = valves 1-8)
    {REG_ID_VALVE_CONFIGURED_0,      REG_ID_VALVE_CONFIGURED_7,      0x02, handle_valve_configured,      "Valve Configured"},

    // Valve states (slot 0x02, registers 0xE0-0xE7 = valves 1-8): 0x00 Off, 0x01 Auto, 0x02 On
    {REG_ID_VALVE_STATE_0,           REG_ID_VALVE_STATE_7,           0x02, handle_valve_state_register,  "Valve State"},

    // Active favourite (slot 0x03, register 0x20): CMD 0x2A value, 0xFF = none active
    {REG_ID_ACTIVE_FAVOURITE,        REG_ID_ACTIVE_FAVOURITE,        0x03, handle_active_favourite,      "Active Favourite"},

    // Favourite enable flags (slot 0x03, registers 0x21-0x28 = Pool,Spa,Fav1-6)
    {REG_ID_FAVOURITE_ENABLE_0,      REG_ID_FAVOURITE_ENABLE_7,      0x03, handle_favourite_enable,      "Favourite Enable"},

    // Water temperature mirror (slot 0x01, register 0x30)
    {REG_ID_WATER_TEMP,              REG_ID_WATER_TEMP,              0x01, handle_water_temp_register,   "Water Temperature"},

    // Favourite labels (slot 0x03, registers 0x31-0x38 = Pool,Spa,Fav1-6)
    {REG_ID_FAVOURITE_LABEL_0,       REG_ID_FAVOURITE_LABEL_7,       0x03, handle_favourite_label,       "Favourite Label"},

    // Solar setpoint (slot 0x01, register 0x3A)
    {REG_ID_SOLAR_SETPOINT,          REG_ID_SOLAR_SETPOINT,         0x01, handle_solar_setpoint,        "Solar Setpoint"},

    // Heater 1 state (slot 0x00, register 0xE6) — touchscreen register-response broadcast.
    // The CMD 0x12 broadcast from 0x0062 is the authoritative state source that updates
    // pool_state->heaters[0]; this entry just names the register-response broadcast so it
    // stops being logged as "Unhandled register".
    {REG_ID_HEATER1_ONOFF,          REG_ID_HEATER1_ONOFF,          0x00, handle_heater1_state,         "Heater 1 State"},

    // Heater 1 setpoints (slot 0x00, registers 0xE7=Pool, 0xE8=Spa)
    {REG_ID_HEATER1_POOL_SETPOINT, REG_ID_HEATER1_SPA_SETPOINT, 0x00, handle_temp_setpoint,          "Heater 1 Setpoint"},

    // Heater 2 state (slot 0x00, register 0xE9): 0x00=Off, 0x01=On
    {REG_ID_HEATER2_ONOFF,         REG_ID_HEATER2_ONOFF,         0x00, handle_heater2_state,          "Heater 2 State"},

    // Heater 1 type (slot 0x00, register 0xC8): none / Genus / Connect Inbuilt
    {REG_ID_HEATER1_TYPE,          REG_ID_HEATER1_TYPE,          0x00, handle_heater1_type,           "Heater 1 Type"},

    // Heater 2 configured flag (slot 0x00, register 0xCC): 0x01 = second heater configured
    {REG_ID_HEATER2_CONFIGURED,    REG_ID_HEATER2_CONFIGURED,    0x00, handle_heater2_configured,     "Heater 2 Configured"},

    // Heater power channels (slot 0x00, registers 0xCA = Heater 1, 0xCD = Heater 2)
    {REG_ID_HEATER1_POWER_CHANNEL, REG_ID_HEATER1_POWER_CHANNEL, 0x00, handle_heater_power_channel,   "Heater 1 Power Channel"},
    {REG_ID_HEATER2_POWER_CHANNEL, REG_ID_HEATER2_POWER_CHANNEL, 0x00, handle_heater_power_channel,   "Heater 2 Power Channel"},

    // Heater 2 setpoints (slot 0x00, registers 0xEA=Pool, 0xEB=Spa)
    {REG_ID_HEATER2_POOL_SETPOINT, REG_ID_HEATER2_SPA_SETPOINT, 0x00, handle_temp_setpoint,          "Heater 2 Setpoint"},

    // Viron Chlorinator unknowns (slot 0x0B, registers 0xE9/0xEA) — meaning
    // unknown; log-only and always recorded as undocumented for research
    {REG_ID_CHLOR_UNKNOWN_COUNTDOWN, REG_ID_CHLOR_UNKNOWN_COUNTDOWN, 0x0B, handle_chlor_unknown_countdown, "Unknown Countdown"},
    {REG_ID_CHLOR_UNKNOWN_VALUE,     REG_ID_CHLOR_UNKNOWN_VALUE,     0x0B, handle_chlor_unknown_value,     "Unknown Value"},

    {REG_ID_MULTICOLOR_LIGHT_TYPE, REG_ID_MULTICOLOR_LIGHT_TYPE, 0x01, handle_multicolor_light_type, "Multicolor Light Type"},

    {REG_ID_CHANNEL_COUNT,         REG_ID_CHANNEL_COUNT,         0x01, handle_channel_count,         "Channel Count"},

    // Channel categories (slot 0x01, registers 0xF5-0xFC = channels 1-8);
    // only broadcast for channels that are in use
    {REG_ID_CHANNEL_CATEGORY_0,    REG_ID_CHANNEL_CATEGORY_7,    0x01, handle_channel_category,      "Channel Category"},
};

#define REGISTER_HANDLER_COUNT (sizeof(REGISTER_HANDLERS) / sizeof(REGISTER_HANDLERS[0]))

// Sensor disconnected / invalid reading sentinel — values >= 0xA0 (160°C) are
// not real water temperatures. Observed e.g. as 0xAF in a Connect 8/10 with
// no sensor wired up.
#define TEMP_INVALID_MIN 0xA0

static inline bool temp_is_invalid(uint8_t t) {
    return t >= TEMP_INVALID_MIN;
}

// Forward declaration — definition is further down with the other registry
// helpers, but handle_temp_reading() (below) needs to call it.
static int find_or_insert_seen_device_locked(pool_state_t *st, uint8_t hi, uint8_t lo);

/**
 * Take the pool state mutex.
 *
 * @param ctx Message decoder context
 * @param what Short description of the state being touched, for the warning
 *             logged when the mutex can't be taken (missing or timed out)
 * @return true if the mutex is held; release it with state_unlock()
 */
static bool state_lock(message_decoder_context_t *ctx, const char *what)
{
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    ESP_LOGW(TAG, "Failed to acquire mutex for %s", what);
    return false;
}

/**
 * Release the pool state mutex taken by state_lock().
 *
 * @param ctx Message decoder context
 * @param touched Stamp last_update_ms before releasing
 * @param snapshot_out If non-NULL, receives a copy of the state (taken after
 *                     the stamp) for publishing once the mutex is released
 */
static void state_unlock(message_decoder_context_t *ctx, bool touched, pool_state_t *snapshot_out)
{
    if (touched) {
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
    if (snapshot_out) {
        *snapshot_out = *ctx->pool_state;
    }
    xSemaphoreGive(ctx->state_mutex);
}

/**
 * Handler: Water temperature reading (CMD 0x16 and CMD 0x31)
 *
 * Source-agnostic and dispatched on the CMD byte. The two CMDs carry the same
 * `{temp1, temp2}` field layout; the only practical difference is the
 * "sensor disconnected" encoding:
 *  - CMD 0x16: a disconnected sensor is reported as `0x00` (indistinguishable
 *    from a genuine 0°C reading — treated as valid by this handler).
 *  - CMD 0x31: a disconnected sensor is reported as `>= 0xA0` (a clean sentinel,
 *    e.g. 0xAF in installations with no sensor wired).
 *
 * Payload layouts (selected by LENGTH byte / payload_len):
 *  - LEN 0x0E (e.g. 0x0062 Connect 8/10): 2-byte payload `{temp1, temp2}` in °C.
 *  - LEN 0x0D (heater devices 0x0070/0x0071/0x0072/0x0074): 1-byte payload `{temp1}`.
 *
 * CMD 0x16 is the canonical source — it writes temp1/temp2 onto the source's
 * `seen_device_t` entry (looked up by the message's source address) and
 * publishes to MQTT. CMD 0x31 is log-only (the Connect 8/10 broadcasts both
 * ~70 ms apart with the same temp1; suppressing 0x31 avoids dual MQTT updates).
 * Values >= 0xA0 are skipped. `single_sensor_source` is committed at first
 * sight from the payload length so the MQTT topic shape stays stable.
 */
static bool handle_temp_reading(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    bool is_canonical = (data[7] == 0x16);
    const char *variant = is_canonical ? "" : " (alt)";

    uint8_t current_temp = payload[0];
    bool t1_invalid = temp_is_invalid(current_temp);

    if (payload_len >= 2) {
        uint8_t current_temp2 = payload[1];
        bool t2_invalid = temp_is_invalid(current_temp2);
        if (t1_invalid || t2_invalid) {
            char t1_buf[24], t2_buf[24];
            if (t1_invalid) snprintf(t1_buf, sizeof t1_buf, "INVALID (raw 0x%02X)", current_temp);
            else            snprintf(t1_buf, sizeof t1_buf, "%d°C", current_temp);
            if (t2_invalid) snprintf(t2_buf, sizeof t2_buf, "INVALID (raw 0x%02X)", current_temp2);
            else            snprintf(t2_buf, sizeof t2_buf, "%d°C", current_temp2);
            ESP_LOGW(TAG, "%s Current temperature%s - %s, temp2: %s",
                     addr_info, variant, t1_buf, t2_buf);
        } else {
            ESP_LOGI(TAG, "%s Current temperature%s - %d°C (temp2: %d°C)",
                     addr_info, variant, current_temp, current_temp2);
        }
    } else {
        if (t1_invalid) {
            ESP_LOGW(TAG, "%s Current temperature%s - INVALID (raw 0x%02X)",
                     addr_info, variant, current_temp);
        } else {
            ESP_LOGI(TAG, "%s Current temperature%s - %d°C", addr_info, variant, current_temp);
        }
    }

    if (!is_canonical) {
        return true;  // CMD 0x31 is log-only.
    }

    // Canonical path (CMD 0x16): write to the source device's temp slots and publish.
    pool_state_t snapshot;
    if (!state_lock(ctx, "temp reading")) return true;

    int dev_idx = find_or_insert_seen_device_locked(ctx->pool_state, data[1], data[2]);
    if (dev_idx >= 0) {
        seen_device_t *dev = &ctx->pool_state->seen_devices[dev_idx];

        // Commit the source's sensor-count shape on first sight. Once set, it
        // stays — keeps MQTT topics stable across the device's lifetime.
        if (!dev->temp1_valid && !dev->temp2_valid) {
            dev->single_sensor_source = (payload_len < 2);
        }

        if (!t1_invalid) {
            dev->temp1 = current_temp;
            dev->temp1_valid = true;
        }
        if (payload_len >= 2 && !dev->single_sensor_source) {
            uint8_t t2 = payload[1];
            if (!temp_is_invalid(t2)) {
                dev->temp2 = t2;
                dev->temp2_valid = true;
            }
        }
    }

    bool publish_temp2 = (dev_idx >= 0)
                      && !ctx->pool_state->seen_devices[dev_idx].single_sensor_source
                      && payload_len >= 2 && !temp_is_invalid(payload[1]);
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        if (!t1_invalid && dev_idx >= 0) {
            mqtt_publish_temperature_reading(&snapshot, dev_idx, 1);
        }
        if (publish_temp2) {
            mqtt_publish_temperature_reading(&snapshot, dev_idx, 2);
        }
    }

    return true;
}

/**
 * Handler: Per-heater temperature setpoint register messages, Slot 0x00.
 * Heater 1: register 0xE7 (Pool), 0xE8 (Spa).
 * Heater 2: register 0xEA (Pool), 0xEB (Spa).
 * °F is derived from °C (registers carry °C only).
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

    // Map register -> (heater index, pool vs spa)
    int heater_idx;
    bool is_pool;
    switch (reg_id) {
        case REG_ID_HEATER1_POOL_SETPOINT: heater_idx = 0; is_pool = true;  break;
        case REG_ID_HEATER1_SPA_SETPOINT: heater_idx = 0; is_pool = false; break;
        case REG_ID_HEATER2_POOL_SETPOINT: heater_idx = 1; is_pool = true;  break;
        case REG_ID_HEATER2_SPA_SETPOINT: heater_idx = 1; is_pool = false; break;
        default:   return false;
    }

    ESP_LOGI(TAG, "%s Heater %d %s setpoint - %d°C", addr_info,
             heater_idx + 1, is_pool ? "Pool" : "Spa", temp_c);

    pool_state_t state_snapshot;
    if (!state_lock(ctx, "temp setpoint")) return true;
    pool_heater_t *heater = &ctx->pool_state->heaters[heater_idx];
    if (is_pool) {
        heater->pool_setpoint   = temp_c;
        heater->pool_setpoint_f = temp_c * 9 / 5 + 32;
        heater->pool_setpoint_valid = true;
    } else {
        heater->spa_setpoint    = temp_c;
        heater->spa_setpoint_f  = temp_c * 9 / 5 + 32;
        heater->spa_setpoint_valid = true;
    }
    state_unlock(ctx, true, &state_snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_heater_setpoints(&state_snapshot, heater_idx);
    }

    return true;
}

/**
 * Handler: Solar temperature setpoint
 * Register 0x3A, Slot 0x01, 1-byte °C value. Log-only — not yet surfaced in
 * pool_state or MQTT.
 */
static bool handle_solar_setpoint(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t temp_c = payload[2];
    ESP_LOGI(TAG, "%s Solar setpoint - %d°C", addr_info, temp_c);

    return true;
}

/**
 * Handler: Current water temperature register mirror
 * Register 0x30, Slot 0x01. The touchscreen mirrors the controller's CMD 0x16
 * reading here for the Internet Gateway to poll; 2-byte payload {temp1, temp2}
 * matching the CMD 0x16 layout. Log-only — the authoritative reading flows
 * through CMD 0x16 (handle_temp_reading), which attributes it to the
 * controller's seen-device entry and publishes to MQTT.
 */
static bool handle_water_temp_register(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t temp_c = payload[2];
    if (payload_len >= 4) {
        ESP_LOGI(TAG, "%s Water temperature (register mirror) - %d°C (temp2: %d°C)",
                 addr_info, temp_c, payload[3]);
    } else {
        ESP_LOGI(TAG, "%s Water temperature (register mirror) - %d°C", addr_info, temp_c);
    }

    return true;
}

/**
 * Handler: Heater 1 state register-response broadcast
 * Register 0xE6, Slot 0x00. Log-only — authoritative state still flows through
 * the CMD 0x12 path (handle_heater) which updates pool_state->heaters[0].
 */
static bool handle_heater1_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t state = payload[2];
    ESP_LOGI(TAG, "%s Heater 1 state - %s (0x%02X)", addr_info,
             state == 0x00 ? "Off" : state == 0x01 ? "On" : "Unknown",
             state);

    // Only Off (0x00) and On (0x01) are documented; flag anything else for research.
    if (state >= 0x02) {
        record_undocumented(data, len);
    }

    return true;
}

/**
 * Handler: Heater 2 state
 * Register 0xE9, Slot 0x00 (0x00=Off, 0x01=On). Authoritative state source for
 * Heater 2 (heaters[1]); Heater 1 (heaters[0]) is driven by CMD 0x12 instead.
 */
static bool handle_heater2_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t state = payload[2];
    ESP_LOGI(TAG, "%s Heater 2 state - %s (0x%02X)", addr_info,
             state == 0x00 ? "Off" : state == 0x01 ? "On" : "Unknown",
             state);

    // Only Off (0x00) and On (0x01) are documented; flag anything else for research.
    if (state >= 0x02) {
        record_undocumented(data, len);
    }

    pool_state_t snapshot;
    if (!state_lock(ctx, "heater 2 state")) return true;
    ctx->pool_state->heaters[1].on = (state != 0);
    ctx->pool_state->heaters[1].valid = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_heater(&snapshot, 1);
    }

    return true;
}

/**
 * Handler: Heater 2 configured flag
 * Register 0xCC, Slot 0x00 (0x01 = second heater configured, 0x00 = not).
 * While set, the Touchscreen's periodic dump carries the Heater 2 registers
 * (0xE9-0xEB) instead of the Heater 1 ones (0xE6-0xE8).
 */
static bool handle_heater2_configured(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t configured = payload[2];

    if (configured >= 0x02) {
        ESP_LOGW(TAG, "%s Heater 2 configured out of expected range 0x00-0x01: 0x%02X", addr_info, configured);
        record_undocumented(data, len);
    }

    ESP_LOGI(TAG, "%s Heater 2 configured - %s", addr_info, configured ? "Yes" : "No");

    pool_state_t snapshot;
    if (state_lock(ctx, "heater 2 configured")) {
        bool changed = !ctx->pool_state->heater2_configured_valid ||
                       ctx->pool_state->heater2_configured != (configured != 0);
        ctx->pool_state->heater2_configured       = (configured != 0);
        ctx->pool_state->heater2_configured_valid = true;
        state_unlock(ctx, true, &snapshot);

        if (changed && ctx->enable_mqtt) {
            mqtt_publish_heater_config(&snapshot, 1);
        }
    }

    return true;
}

/**
 * Handler: Heater 1 type
 * Register 0xC8, Slot 0x00 (0x00 = none, 0x01 = Genus (Gas), 0x03 = Connect Inbuilt).
 */
static bool handle_heater1_type(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t type = payload[2];
    const char *name;
    switch (type) {
        case HEATER1_TYPE_NONE:            name = "None"; break;
        case HEATER1_TYPE_GENUS:           name = "Genus (Gas)"; break;
        case HEATER1_TYPE_CONNECT_INBUILT: name = "Connect Inbuilt"; break;
        default:
            name = "Unknown";
            record_undocumented(data, len);
            break;
    }

    ESP_LOGI(TAG, "%s Heater 1 type - %s (%d)", addr_info, name, type);

    pool_state_t snapshot;
    if (state_lock(ctx, "heater 1 type")) {
        bool changed = !ctx->pool_state->heater1_type_valid ||
                       ctx->pool_state->heater1_type != type;
        ctx->pool_state->heater1_type       = type;
        ctx->pool_state->heater1_type_valid = true;
        state_unlock(ctx, true, &snapshot);

        if (changed && ctx->enable_mqtt) {
            mqtt_publish_heater_config(&snapshot, 0);
        }
    }

    return true;
}

/**
 * Handler: Heater power channel
 * Register 0xCA (Heater 1), payload {0x00, channel}; register 0xCD (Heater 2),
 * payload {0x00, channel, 0x00}. Slot 0x00. The channel is the 0-based index
 * of the controller channel powering a Connect Inbuilt heater; 0xFF (seen on
 * Heater 2 only) means none assigned. The 0x00 bytes have only ever read 0x00.
 * Log-only.
 */
static bool handle_heater_power_channel(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    bool is_heater2 = (payload[0] == REG_ID_HEATER2_POWER_CHANNEL);
    if (payload_len < (is_heater2 ? 5 : 4)) return false;

    int heater_num  = is_heater2 ? 2 : 1;
    uint8_t channel = payload[3];

    if (channel == 0xFF) {
        ESP_LOGI(TAG, "%s Heater %d power channel - none", addr_info, heater_num);
    } else {
        ESP_LOGI(TAG, "%s Heater %d power channel - Ch%d", addr_info, heater_num, channel + 1);
    }

    // PROTOCOL.md Appendix A 0xCA/0xCD: padding bytes only seen as 0x00, the
    // channel within the 8 channel slots, and 0xFF only on Heater 2.
    bool undocumented = payload[2] != 0x00 ||
                        (is_heater2 && payload[4] != 0x00) ||
                        (channel == 0xFF ? !is_heater2 : channel >= MAX_CHANNELS);
    if (undocumented) {
        record_undocumented(data, len);
    }

    return true;
}

/**
 * Handler: Viron Chlorinator unknown countdown
 * Register 0xE9, Slot 0x0B. 2-byte little-endian countdown in 1/5 s units
 * (300 = 1 minute) + 2 bytes only ever seen as 00 00. What it counts down to
 * is unknown, so it is log-only and every frame is recorded as undocumented.
 */
static bool handle_chlor_unknown_countdown(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 4) return false;

    uint16_t units = (uint16_t)(payload[2] | (payload[3] << 8));
    ESP_LOGI(TAG, "%s Unknown countdown - %u (%u min %u s)", addr_info,
             units, units / 300, (units % 300) / 5);

    record_undocumented(data, len);
    return true;
}

/**
 * Handler: Viron Chlorinator unknown value
 * Register 0xEA, Slot 0x0B. 2-byte little-endian value + 2 bytes only ever
 * seen as 00 00. Static per install (4500 and 9000 observed); meaning
 * unknown, so it is log-only and every frame is recorded as undocumented.
 */
static bool handle_chlor_unknown_value(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 4) return false;

    uint16_t value = (uint16_t)(payload[2] | (payload[3] << 8));
    ESP_LOGI(TAG, "%s Unknown value - %u", addr_info, value);

    record_undocumented(data, len);
    return true;
}

/**
 * Handler: Multicolor light type selection
 * Register 0xF0, Slot 0x01
 * System-wide light model selected in the touchscreen's light setup:
 * 0x00=SLX, 0x01=Delta, 0xFF=no multicolor light configured. A value outside
 * the known set is a new model index worth capturing.
 */
static bool handle_multicolor_light_type(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t type = payload[2];
    char name_buf[16];
    const char *name = multicolor_light_type_name(type, name_buf, sizeof(name_buf));

    ESP_LOGI(TAG, "%s Multicolor light type - %s", addr_info, name);

    if (type != MULTICOLOR_LIGHT_TYPE_NONE &&
        type != MULTICOLOR_LIGHT_TYPE_SLX &&
        type != MULTICOLOR_LIGHT_TYPE_DELTA) {
        record_undocumented(data, len);
    }

    bool type_changed = false;
    pool_state_t state_snapshot;

    if (state_lock(ctx, "multicolor light type")) {
        type_changed = !ctx->pool_state->multicolor_light_type_valid ||
                       ctx->pool_state->multicolor_light_type != type;
        ctx->pool_state->multicolor_light_type = type;
        ctx->pool_state->multicolor_light_type_valid = true;
        state_unlock(ctx, true, &state_snapshot);
    }

    // The type selects the color effect list on the HA light entities, so
    // re-publish configured zones when it changes (refreshes their discovery)
    if (type_changed && ctx->enable_mqtt) {
        for (int i = 0; i < MAX_LIGHT_ZONES; i++) {
            if (state_snapshot.lighting[i].configured) {
                mqtt_publish_light(&state_snapshot, state_snapshot.lighting[i].zone);
            }
        }
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

    if (state_lock(ctx, "channel count")) {
        ctx->pool_state->num_channels = count;
        state_unlock(ctx, true, NULL);
    }

    return true;
}

/**
 * Handler: Gas heater device status (CMD 0x12, source 0x0071 Viron,
 * 0x0072 HiNRG or 0x0074 ICI)
 *
 * Same four-byte payload shape from either gas heater; addr_info already names
 * the source device. All bytes are 0x00 when the heater is idle (modulation=0).
 * Full byte meanings when actively heating are not yet decoded.
 */
static bool handle_gas_heater_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 4) return false;


    const uint8_t raw_status = payload[1];
    const uint8_t functional_status = raw_status & GAS_HEATER_BITMASK_FUNCTIONAL_STATUS;
    gas_heater_status_t heater_status;

    switch (functional_status) {
        case 0x00:
            heater_status = HEATER_OFF;
            break;
        case 0x01:
            heater_status = HEATER_ON_NO_FLOW;
            break;
        case 0x02:
            heater_status = HEATER_OFF;
            break;
        case 0x03:
            heater_status = HEATER_SETPOINT_REACHED;
            break;
        case 0x07:
            heater_status = HEATER_IGNITING;
            break;
        case 0x0F:
            heater_status = HEATER_HEATING;
            break;
        case 0x12:
            heater_status = HEATER_COOLDOWN;
            break;
        case 0x13:
            heater_status = HEATER_LOCKED_OUT;
            break;
        default:
            // Recognised gas-heater frame, but this functional-status value is
            // not in the documented set (PROTOCOL.md 0x12 gas-heater table).
            ESP_LOGW(TAG, "%s Undocumented gas heater status: 0x%02X", addr_info, functional_status);
            record_undocumented(data, len);
            return true;
    }

    // Bytes 10, 12 and 13 are always 0x00 in observed captures.
    if (payload[0] != 0x00 || payload[2] != 0x00 || payload[3] != 0x00) {
        record_undocumented(data, len);
    }

    const bool heater_on = (raw_status & GAS_HEATER_BITMASK_HEATER_ON) != 0;
    const bool water_flow_detected = (raw_status & GAS_HEATER_BITMASK_WATER_FLOW) != 0;
    const bool gas_valve_open = (raw_status & GAS_HEATER_BITMASK_GAS_VALVE) != 0;
    const bool burner_alight = (raw_status & GAS_HEATER_BITMASK_BURNER_ALIGHT) != 0;
    const bool locked_out = (raw_status & GAS_HEATER_BITMASK_LOCKED_OUT) != 0;
    const bool general_service_required = (raw_status & GAS_HEATER_BITMASK_GENERAL_SERVICE_REQUIRED) != 0;
    const bool ignition_service_required = (raw_status & GAS_HEATER_BITMASK_IGNITION_SERVICE_REQUIRED) != 0;
    const bool cooling_available = (raw_status & GAS_HEATER_BITMASK_COOLING_AVAILABLE) != 0;

    gas_heater_burner_state_t burner_state;
    if (!gas_valve_open && !burner_alight) {
        burner_state = BURNER_OFF;
    } else if (gas_valve_open && !burner_alight) {
        burner_state = BURNER_IGNITING;
    } else if (gas_valve_open && burner_alight) {
        burner_state = BURNER_ALIGHT;
    } else {
        // gas_valve closed but burner alight — an undocumented combination that
        // should never occur given the valid status filter above. Capture it for
        // research and count the frame as decoded.
        ESP_LOGW(TAG, "%s Undocumented gas burner state: gas_valve=%d, alight=%d", addr_info, gas_valve_open, burner_alight);
        record_undocumented(data, len);
        return true;
    }

    ESP_LOGI(TAG, "%s Gas heater raw status - [%02X %02X %02X %02X], decoded status - \"%s\"",
             addr_info, payload[0], payload[1], payload[2], payload[3], HEATER_STATUS_NAMES[heater_status]);
    if (general_service_required || ignition_service_required) {
        ESP_LOGI(TAG, "%s Gas heater service required - general %s, ignition %s",
            addr_info, general_service_required ? "true" : "false", ignition_service_required ? "true" : "false");
    }

    // Update state and publish. A Genus (smart-control) heater on the bus can
    // only be Heater 1: a second heater is always Connect Inbuilt, switched
    // through a controller channel rather than a device of its own.
    pool_state_t snapshot;
    if (!state_lock(ctx, "gas heater status")) return true;
    pool_heater_t* const heater_state = &(ctx->pool_state->heaters[0]);
    heater_state->valid = true;
    heater_state->on = heater_on;

    heater_state->gas_heater_valid = true;
    heater_state->water_flow_detected = water_flow_detected;
    heater_state->locked_out = locked_out;
    heater_state->burner_state = burner_state;
    heater_state->general_service_required = general_service_required;
    heater_state->ignition_service_required = ignition_service_required;
    heater_state->cooling_available = cooling_available;
    heater_state->status = heater_status;

    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_heater(&snapshot, 0);
        mqtt_publish_gas_heater(&snapshot, 0);
    }

    return true;
}

/**
 * Handler: Genus Heater (Active i25 Evo heat pump) device status (CMD 0x12,
 * source 0x0070)
 *
 * Same four-byte payload shape as the gas heaters ({00, status, 00, 00}; the
 * data checksum equals the status byte) and the same bit 0 (heater on) /
 * bit 1 (water flow) semantics, but the upper status bits do not follow the
 * gas-heater table — a heat pump has no gas valve or flame. 0x13 is broadcast
 * while the unit heats normally (observed ~360 ms after a Gateway heater-on
 * register write), so bit 4 is believed to mean "actively heating" here, not
 * the gas heaters' lockout. Only the observed value set is accepted; anything
 * else — including non-zero padding bytes — is recorded as undocumented.
 */
static bool handle_genus_heater_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 4) return false;

    const uint8_t status = payload[1];
    const char *status_name;

    switch (status) {
        case 0x00: status_name = "Off (no water flow)"; break;
        case 0x02: status_name = "Off (water flow)"; break;
        case 0x03: status_name = "Setpoint Reached"; break;
        case 0x07: status_name = "Starting Up (unconfirmed)"; break;
        case 0x13: status_name = "Heating (unconfirmed)"; break;
        default:
            // Recognised Genus heater frame, but this status value is not in
            // the documented set (PROTOCOL.md 0x12, Genus Heater variant).
            ESP_LOGW(TAG, "%s Undocumented Genus heater status: 0x%02X", addr_info, status);
            record_undocumented(data, len);
            return true;
    }

    // Bytes 10, 12 and 13 are always 0x00 in observed captures.
    if (payload[0] != 0x00 || payload[2] != 0x00 || payload[3] != 0x00) {
        record_undocumented(data, len);
    }

    const bool heater_on = (status & GAS_HEATER_BITMASK_HEATER_ON) != 0;

    ESP_LOGI(TAG, "%s Genus heater raw status - [%02X %02X %02X %02X], decoded status - \"%s\"",
             addr_info, payload[0], payload[1], payload[2], payload[3], status_name);

    // Update state and publish. The Genus is Heater 1 on systems that carry
    // it (the Gateway drives it via register 0xE6, Heater 1 On/Off), as any
    // Genus (smart-control) heater on the bus must be: a second heater is
    // always Connect Inbuilt.
    pool_state_t snapshot;
    if (!state_lock(ctx, "Genus heater status")) return true;
    ctx->pool_state->heaters[0].on = heater_on;
    ctx->pool_state->heaters[0].valid = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_heater(&snapshot, 0);
    }

    return true;
}

/**
 * Handler: Genus / Viron / HiNRG Gas Heater / ICI Gas Heater setpoints
 * Pattern (`0x0070`): `02 00 70 FF FF 80 00 17 0E 15` (Genus Heater)
 * Pattern (`0x0071`): `02 00 71 FF FF 80 00 17 0E 16` (Viron Gas Heater)
 * Pattern (`0x0072`): `02 00 72 FF FF 80 00 17 0E 17` (HiNRG Gas Heater)
 * Pattern (`0x0074`): `02 00 74 FF FF 80 00 17 0E 19` (ICI Gas Heater)
 *
 * Two-byte payload: byte 10 = Spa setpoint (°C), byte 11 = Pool setpoint (°C).
 *
 * On an install with no Internet Gateway and a touchscreen that never sends
 * CMD 0x17, this is the only message carrying a setpoint changed at the
 * touchscreen or on the heater's own panel: the register requester fetches
 * 0xE7/0xE8/0xEA/0xEB once and then stops, so the heater's broadcast (~every
 * 60 s) is what keeps the value current afterwards.
 *
 * The frame does not identify which controller heater slot the device occupies,
 * and it is not always Heater 1 — on the two-heater install in PROTOCOL.md
 * Appendix A the Genus heat pump's values match 0xEA/0xEB. So the values are
 * applied only once register 0xCC says there is no second heater. While one is
 * configured, or before 0xCC has been received, this stays log-only, leaving
 * handle_temp_setpoint authoritative.
 *
 * A heater that isn't plumbed to both circuits broadcasts the 0x0A (10°C)
 * "uninstalled" default in the unused circuit, so each circuit is applied only
 * if it isn't exactly that value.
 */
static bool handle_heater_temp_setting(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t spa_setpoint  = payload[0];
    uint8_t pool_setpoint = payload[1];

    uint8_t device_hi = data[1], device_lo = data[2];
    char heater_name_buf[16];
    const char *heater_name = get_device_name(device_hi, device_lo, heater_name_buf, sizeof(heater_name_buf));

    ESP_LOGI(TAG, "%s %s setpoints - Spa=%d°C, Pool=%d°C",
             addr_info, heater_name, spa_setpoint, pool_setpoint);

    pool_state_t snapshot;
    if (!state_lock(ctx, "heater device setpoints")) return true;

    bool heater2_known;
    bool heater2_present = heater_configured(ctx->pool_state, 1, &heater2_known);
    if (!heater2_known || heater2_present) {
        state_unlock(ctx, false, NULL);
        ESP_LOGD(TAG, "%s setpoints not applied - %s", heater_name,
                 heater2_known ? "Heater 2 configured, heater slot unknown"
                               : "heater configuration not yet known");
        return true;
    }

    pool_heater_t *heater = &ctx->pool_state->heaters[0];
    bool updated = false;
    if (spa_setpoint != TEMP_SETPOINT_MIN_C) {
        heater->spa_setpoint       = spa_setpoint;
        heater->spa_setpoint_f     = spa_setpoint * 9 / 5 + 32;
        heater->spa_setpoint_valid = true;
        updated = true;
    }
    if (pool_setpoint != TEMP_SETPOINT_MIN_C) {
        heater->pool_setpoint       = pool_setpoint;
        heater->pool_setpoint_f     = pool_setpoint * 9 / 5 + 32;
        heater->pool_setpoint_valid = true;
        updated = true;
    }
    if (!updated) {
        state_unlock(ctx, false, NULL);
        return true;
    }
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_heater_setpoints(&snapshot, 0);
    }

    return true;
}

/**
 * Handler: Connect 8/10 Controller device status (CMD 0x12, source 0x0062)
 *
 * The controller's own view of Heater 1. Its heater bit is used only when
 * register 0xC8 says Heater 1 is Connect Inbuilt, i.e. powered through a
 * controller channel that the controller switches. A Genus (smart-control)
 * heater (Genus 0x0070, Viron 0x0071, HiNRG 0x0072, ICI 0x0074) reports its
 * own CMD 0x12 status, which is authoritative: this bit has been observed
 * stuck at 0 for the whole time such a heater was running (payload
 * `04 00 00`). Before 0xC8 has been received the bit is not used. Service
 * mode comes from here either way.
 */
static bool handle_heater(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t status = payload[1];
    bool heater_on    = (status & 0x01) != 0;
    bool service_mode = (status & 0x02) != 0;
    // Captured against physical stop/start: clears when Solar stops, sets in
    // Auto when the pump starts, and stays set when Auto changes to On.
    bool solar_active = (status & 0x04) != 0;
    ESP_LOGI(TAG, "%s Heater - %s, Service mode - %s, Solar output - %s",
             addr_info, heater_on ? "On" : "Off", service_mode ? "On" : "Off",
             solar_active ? "Active" : "Inactive");

    // Flag undocumented field values for protocol research (PROTOCOL.md 0x12,
    // Connect 8/10 variant). Byte 10 is unknown, observed as 0x00 or 0x04;
    // byte 11 a status bitfield (bit 0 heater, bit 1 service, bit 2 Solar; higher bits
    // unseen); byte 12 unknown, observed as 0x00 or 0x08. Any other value is
    // surfaced for capture.
    bool undocumented = (payload[0] != 0x00 && payload[0] != 0x04) || (status & ~0x07) != 0;
    if (payload_len >= 3 && payload[2] != 0x00 && payload[2] != 0x08) {
        undocumented = true;
    }
    if (undocumented) {
        record_undocumented(data, len);
    }

    // Update state and publish
    pool_state_t snapshot;
    if (!state_lock(ctx, "heater state")) return true;
    bool heater_is_ours = ctx->pool_state->heater1_type_valid &&
                          ctx->pool_state->heater1_type == HEATER1_TYPE_CONNECT_INBUILT;
    if (heater_is_ours) {
        ctx->pool_state->heaters[0].on = heater_on;
        ctx->pool_state->heaters[0].valid = true;
    }
    ctx->pool_state->service_mode = service_mode;
    ctx->pool_state->service_mode_valid = true;
    ctx->pool_state->solar_active = solar_active;
    ctx->pool_state->solar_active_valid = true;
    ctx->pool_state->solar_status_byte = status;
    ctx->pool_state->solar_status_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint8_t solar_channels[MAX_CHANNELS];
    int solar_count = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        channel_state_t *ch = &ctx->pool_state->channels[i];
        if (ch->configured && apply_solar_status(ctx->pool_state, ch)) {
            solar_channels[solar_count++] = (uint8_t)(i + 1);
        }
    }
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        if (heater_is_ours) {
            mqtt_publish_heater(&snapshot, 0);
        }
        mqtt_publish_service_mode(&snapshot);
        for (int i = 0; i < solar_count; i++) {
            mqtt_publish_channel(&snapshot, solar_channels[i]);
        }
    }

    return true;
}

/**
 * Handler: Temperature setting message — Touchscreen (0x0050) broadcast.
 * Pattern: "02 00 50 FF FF 80 00 17 10 F7"
 *
 * This is the controller's own setpoint broadcast (carries real °C and °F for
 * both circuits). Routed to Heater 1 (heaters[0]) — the primary heater whose
 * setpoints the controller exposes via registers 0xE7/0xE8.
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

    ESP_LOGI(TAG, "%s Heater 1 setpoints - spa=%d°C/%d°F, pool=%d°C/%d°F",
             addr_info, spa_set_temp_c, spa_set_temp_f, pool_set_temp_c, pool_set_temp_f);

    // Update state and publish
    pool_state_t snapshot;
    if (!state_lock(ctx, "temp setting")) return true;
    pool_heater_t *heater1 = &ctx->pool_state->heaters[0];
    heater1->spa_setpoint    = spa_set_temp_c;
    heater1->pool_setpoint   = pool_set_temp_c;
    heater1->spa_setpoint_f  = spa_set_temp_f;
    heater1->pool_setpoint_f = pool_set_temp_f;
    heater1->pool_setpoint_valid = true;
    heater1->spa_setpoint_valid  = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_heater_setpoints(&snapshot, 0);
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
    const char *mode_str = (mode == MODE_SPA) ? "Spa" : (mode == MODE_POOL) ? "Pool" : "Unknown";
    ESP_LOGI(TAG, "%s Mode - %s", addr_info, mode_str);

    // Only MODE_SPA and MODE_POOL are documented; flag anything else.
    if (mode > MODE_POOL) {
        record_undocumented(data, len);
    }

    // Update state and publish
    pool_state_t snapshot;
    if (!state_lock(ctx, "mode")) return true;
    ctx->pool_state->mode = mode;
    ctx->pool_state->mode_valid = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_mode(&snapshot);
    }

    return true;
}

/**
 * Handler: Mode set command (CMD 0x15) — source-agnostic.
 * Two forms, told apart by payload length:
 *  - 1 byte (Touchscreen 0x0050): sets the mode; same encoding as the 0x14
 *    status (0x00 = Spa, 0x01 = Pool). Mode is updated optimistically — the
 *    touchscreen broadcasts a 0x14 status to confirm.
 *  - No payload (frame with no data, from the Connect 8/10 0x0062 Pool/Spa
 *    button and the ICI Gas Heater 0x0074): toggles the mode. Log-only, as
 *    the frame names no target — the new mode comes from the 0x14 that
 *    follows.
 */
static bool handle_mode_set_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len == 0) {
        ESP_LOGI(TAG, "%s Mode toggle command", addr_info);
        return true;
    }

    uint8_t mode = payload[0];
    const char *mode_str = (mode == MODE_SPA) ? "Spa" : (mode == MODE_POOL) ? "Pool" : "Unknown";
    ESP_LOGI(TAG, "%s Mode set command - %s", addr_info, mode_str);

    // Only MODE_SPA and MODE_POOL are documented; flag anything else.
    if (mode > MODE_POOL) {
        record_undocumented(data, len);
    }

    pool_state_t snapshot;
    if (!state_lock(ctx, "mode set command")) return true;
    ctx->pool_state->mode = mode;
    ctx->pool_state->mode_valid = true;
    state_unlock(ctx, true, &snapshot);

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
    const char *scale_str        = (config_byte & 0x10) ? "Fahrenheit"   : "Celsius";
    const char *step_str         = (config_byte & 0x04) ? "2°"           : "1°";
    const char *heater_active_str = (config_byte & 0x08) ? "On"          : "Off";
    const char *mode_str         = (config_byte & 0x02) ? "cooler-only"  : "heat";
    ESP_LOGI(TAG, "%s Config - temperature scale=%s, step=%s, heater=%s, mode=%s",
             addr_info, scale_str, step_str, heater_active_str, mode_str);

    // Update state only (no MQTT publishing)
    if (state_lock(ctx, "config")) {
        ctx->pool_state->temp_scale_fahrenheit = (config_byte & 0x10) != 0;
        state_unlock(ctx, true, NULL);
    }

    // Flag anomalous config bytes for protocol research (e.g. a service-mode
    // signal hiding in an undocumented bit; see PROTOCOL.md 0x26 byte 10).
    // Documented bits are the low five (mask 0x1F): bit 0 (0x01) is documented
    // as always 1, bits 1-4 carry the fields decoded above. Bits 5-7 (0xE0) are
    // reserved/unknown. Record — outside the state lock, per record_undocumented's
    // contract — if any reserved bit is set, or if the "always 1" bit 0 is
    // unexpectedly clear. Called only on anomalies so normal frames don't flood
    // the Unknown Messages page.
    if ((config_byte & 0xE0) != 0 || (config_byte & 0x01) == 0) {
        ESP_LOGW(TAG, "%s Undocumented config bits: 0x%02X", addr_info, config_byte);
        record_undocumented(data, len);
    }

    return true;
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

    if (minutes > 59 || hours > 23 || day_of_week > 6) {
        ESP_LOGE(TAG, "%s Invalid controller time - %02d:%02d day:%d", addr_info, hours, minutes, day_of_week);
        record_undocumented(data, len);
        return true;
    }

    const char *day_name = (day_of_week < DAY_OF_WEEK_COUNT) ? DAY_OF_WEEK_NAMES[day_of_week] : "Unknown";
    ESP_LOGI(TAG, "%s Controller time - %02d:%02d %s", addr_info, hours, minutes, day_name);

    // Update state only (no MQTT publishing)
    if (state_lock(ctx, "controller time")) {
        ctx->pool_state->controller_minutes = minutes;
        ctx->pool_state->controller_hours = hours;
        ctx->pool_state->controller_day_of_week = day_of_week;
        ctx->pool_state->controller_time_valid = true;
        state_unlock(ctx, true, NULL);
    }

    return true;
}

// Find or insert a seen-device entry. Returns slot index, or -1 if full.
// Caller must hold the pool-state mutex.
static int find_or_insert_seen_device_locked(pool_state_t *st, uint8_t hi, uint8_t lo)
{
    for (int i = 0; i < st->num_seen_devices; i++) {
        if (st->seen_devices[i].addr_hi == hi && st->seen_devices[i].addr_lo == lo) {
            return i;
        }
    }
    if (st->num_seen_devices >= MAX_SEEN_DEVICES) return -1;
    int idx = st->num_seen_devices++;
    st->seen_devices[idx].addr_hi = hi;
    st->seen_devices[idx].addr_lo = lo;
    st->seen_devices[idx].fw_version_valid = false;
    st->seen_devices[idx].fw_version_major = 0;
    st->seen_devices[idx].fw_version_minor = 0;
    st->seen_devices[idx].decoded_count = 0;
    st->seen_devices[idx].unknown_count = 0;
    st->seen_devices[idx].temp1 = 0;
    st->seen_devices[idx].temp2 = 0;
    st->seen_devices[idx].temp1_valid = false;
    st->seen_devices[idx].temp2_valid = false;
    st->seen_devices[idx].single_sensor_source = false;
    return idx;
}

/**
 * Handler: Firmware version (CMD 0x0A, source-agnostic)
 *
 * Covers PROTOCOL.md command `0x0A` (consolidated). Same 2-byte `{major, minor}`
 * payload shape across every observed source — the source address selects
 * which `pool_state->*_version_*` field is populated.
 *
 * Unknown sources are logged but skipped for state-update purposes.
 */
static bool handle_firmware_version(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t major = payload[0];
    uint8_t minor = payload[1];
    uint16_t src = ((uint16_t)data[1] << 8) | data[2];

    ESP_LOGI(TAG, "%s Firmware version - %d.%d", addr_info, major, minor);

    if (!state_lock(ctx, "firmware version")) return true;

    switch (src) {
        case 0x0050:
            ctx->pool_state->touchscreen_version_major = major;
            ctx->pool_state->touchscreen_version_minor = minor;
            ctx->pool_state->touchscreen_version_valid = true;
            break;
        case 0x0062:
            ctx->pool_state->controller_version_major = major;
            ctx->pool_state->controller_version_minor = minor;
            ctx->pool_state->controller_version_valid = true;
            break;
        case 0x0084:
            ctx->pool_state->chlor_version_major = major;
            ctx->pool_state->chlor_version_minor = minor;
            ctx->pool_state->chlor_version_valid = true;
            break;
        case 0x00F0:
            ctx->pool_state->gateway_version_major = major;
            ctx->pool_state->gateway_version_minor = minor;
            ctx->pool_state->gateway_version_valid = true;
            break;
        // 0x0070 (Genus Heater) and any future device — log-only, no dedicated state field yet
    }

    int dev_idx = find_or_insert_seen_device_locked(ctx->pool_state, data[1], data[2]);
    if (dev_idx >= 0) {
        ctx->pool_state->seen_devices[dev_idx].fw_version_valid = true;
        ctx->pool_state->seen_devices[dev_idx].fw_version_major = major;
        ctx->pool_state->seen_devices[dev_idx].fw_version_minor = minor;
    }

    state_unlock(ctx, true, NULL);

    return true;
}

/**
 * Handler: Touchscreen device status (CMD 0x12, source 0x0050)
 *
 * The Touchscreen's own entry in the CMD 0x12 device-status family, sent
 * alongside its CMD 0x0A firmware version. Byte 10 has bit 0 always set and a
 * value of 0-2 in bits 2-3 (0x01/0x05/0x09 observed); what that field means
 * is unknown. Byte 11 is always 0x00. Log-only.
 */
static bool handle_touchscreen_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t data_byte1 = payload[0];
    uint8_t data_byte2 = payload[1];

    if ((data_byte1 != 0x01 && data_byte1 != 0x05 && data_byte1 != 0x09) || data_byte2 != 0x00) {
        ESP_LOGW(TAG, "%s Touchscreen status - UNEXPECTED VALUE: Byte1: 0x%02X (%d), Byte2: 0x%02X (%d) (expected 0x01|0x05|0x09 0x00)",
                 addr_info, data_byte1, data_byte1, data_byte2, data_byte2);
        record_undocumented(data, len);
    } else {
        ESP_LOGI(TAG, "%s Touchscreen status - 0x%02X (bits 3-2 = %d)",
                 addr_info, data_byte1, (data_byte1 >> 2) & 0x03);
    }

    return true;
}

/**
 * Handler: Unknown/unhandled message
 *
 * Logs everything we *can* identify about an unknown message so it can be
 * triaged from logs without a full hex re-read:
 *   - addr_info: source/destination, resolved to device names when known
 *   - CMD byte (data[7]): the protocol command byte
 *   - LEN byte (data[8]): the frame's declared length
 *   - payload bytes only (between header and frame checksum)
 *
 * The full raw frame is already emitted as "RX MSG" by decode_message()
 * before dispatch, so it's not repeated here.
 */
static bool handle_unknown(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    // Format payload bytes as hex string (data section only)
    char payload_hex[3 * BUS_MESSAGE_MAX_SIZE + 1];
    int pos = 0;
    for (int i = 0; i < payload_len && pos < (int)sizeof(payload_hex) - 3; i++) {
        pos += snprintf(&payload_hex[pos], sizeof(payload_hex) - pos, "%02X ", payload[i]);
    }
    // Strip trailing space if any payload was written
    if (pos > 0) payload_hex[pos - 1] = '\0';
    else payload_hex[0] = '\0';

    uint8_t cmd = data[7];
    uint8_t length_byte = data[8];

    char cmd_name_buf[24];
    const char *cmd_name = get_cmd_name(cmd, cmd_name_buf, sizeof(cmd_name_buf));

    ESP_LOGW(TAG, "Unhandled %s CMD=0x%02X (%s) LEN=%u payload=[%s]",
             addr_info, cmd, cmd_name, length_byte, payload_hex);

    return false;  // Not decoded
}

/**
 * Handler: Touchscreen unknown broadcast (CMD 0x05)
 * Invariant across all captures: data byte always 0x00 or 0x01.
 * Silenced here to avoid spurious "Unhandled" warnings.
 */
static bool handle_touchscreen_unknown3(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    uint8_t ack = (payload_len > 0) ? payload[0] : 0;
    // PROTOCOL.md 0x05: byte 10 is observed only as 0x00 or 0x01. Flag anything
    // else (unconfirmed whether it is a fixed ack sentinel or a flags field).
    if (payload_len > 0 && ack > 0x01) {
        ESP_LOGW(TAG, "%s Touchscreen unknown (CMD 0x05) - UNEXPECTED VALUE: 0x%02X (expected 0x00|0x01)",
                 addr_info, ack);
        record_undocumented(data, len);
    } else {
        ESP_LOGI(TAG, "%s Touchscreen unknown (CMD 0x05): 0x%02X", addr_info, ack);
    }
    return true;
}

/**
 * Handler: Valve state broadcast (CMD 0x27)
 * Byte 10: slot count; then 3 bytes per slot: [configured][state][active].
 * The count is 0 (payload `00`, LEN 0x0D) while no valve zone is configured,
 * and jumps to the full slot count (LEN 0x13) once any one is.
 */
static bool handle_valve_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t slot_count = payload[0];
    if (slot_count > MAX_VALVE_SLOTS) {
        // Only 0 and 2 slots have been seen; more would mean a third valve zone.
        ESP_LOGW(TAG, "%s Valve state - %d slots, only the first %d decoded",
                 addr_info, slot_count, MAX_VALVE_SLOTS);
        record_undocumented(data, len);
        slot_count = MAX_VALVE_SLOTS;
    }

    if (payload_len < 1 + slot_count * 3) {
        ESP_LOGW(TAG, "%s Valve state - truncated payload (slots=%d, payload_len=%d)",
                 addr_info, slot_count, payload_len);
        record_undocumented(data, len);
        return true;
    }

    // Log each slot before taking the mutex, in the same layout as
    // handle_channel_status
    ESP_LOGI(TAG, "%s Valve state (%d slots):", addr_info, slot_count);
    bool undocumented_state = false;
    for (int i = 0; i < slot_count; i++) {
        bool configured = (payload[1 + i * 3] == 0x01);
        uint8_t state   = payload[2 + i * 3];
        bool active     = (payload[3 + i * 3] == 0x01);
        if (configured) {
            const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
            ESP_LOGI(TAG, "  V%d: %s (%s)", i + 1, state_name, active ? "Active" : "Inactive");
            // Valves only use Off/Auto/On (0x00-0x02); flag anything else.
            if (state > 0x02) undocumented_state = true;
        } else {
            ESP_LOGI(TAG, "  V%d: Unused", i + 1);
        }
    }
    if (undocumented_state) {
        record_undocumented(data, len);
    }

    bool changed = false;
    bool new_valve_configured = false;
    pool_state_t state_snapshot;
    if (state_lock(ctx, "valve state")) {
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
        state_unlock(ctx, changed, &state_snapshot);
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
 * Handler: Internet Gateway serial number (CMD 0x37 item 0x04)
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
    if (state_lock(ctx, "serial number")) {
        ctx->pool_state->serial_number = serial;
        ctx->pool_state->serial_number_valid = true;
        state_unlock(ctx, true, NULL);
    }

    return true;
}

/**
 * Handler: Internet Gateway info request (CMD 0x36) — source-agnostic.
 * Sent by the Touchscreen (0x0050) to the Gateway (0x00F0). payload[0] is the
 * item, answered by the CMD 0x37 variant with the same byte 10: 0x01 = network
 * config (payload[1] = interface, 0x00 Wired / 0x01 WiFi), 0x02 = comms status,
 * 0x04 = serial number, 0x05 = firmware version. 0x03 is the Touchscreen's
 * Reconnect button, a command with no reply. Log-only.
 */
static bool handle_gateway_info_request(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t item = payload[0];
    bool undocumented = false;

    switch (item) {
        case 0x01:
            if (payload_len < 2) return false;
            ESP_LOGI(TAG, "%s Internet Gateway info request - Network config (%s)", addr_info,
                     payload[1] == 0x00 ? "Wired" : payload[1] == 0x01 ? "WiFi" : "Unknown interface");
            undocumented = payload_len != 2 || payload[1] > 0x01;
            break;
        case 0x02:
            ESP_LOGI(TAG, "%s Internet Gateway info request - Comms status", addr_info);
            undocumented = payload_len != 1;
            break;
        case 0x03:
            ESP_LOGI(TAG, "%s Internet Gateway reconnect command", addr_info);
            undocumented = payload_len != 1;
            break;
        case 0x04:
            ESP_LOGI(TAG, "%s Internet Gateway info request - Serial number", addr_info);
            undocumented = payload_len != 1;
            break;
        case 0x05:
            ESP_LOGI(TAG, "%s Internet Gateway info request - Firmware version", addr_info);
            undocumented = payload_len != 1;
            break;
        default:
            ESP_LOGI(TAG, "%s Internet Gateway info request - Unknown item (0x%02X)", addr_info, item);
            undocumented = true;
            break;
    }

    // PROTOCOL.md 0x36: items 0x01-0x05 only, interface 0x00/0x01 only.
    if (undocumented) {
        record_undocumented(data, len);
    }

    return true;
}

/**
 * Handler: Internet Gateway network config (CMD 0x37 item 0x01)
 * payload[1] is the interface (GATEWAY_IFACE_WIRED / GATEWAY_IFACE_WIFI), and
 * each interface's config is stored separately.
 */
static bool handle_gateway_ip(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 9) return false;

    // Interface in payload[1], status flags in payload[3], IP address in
    // payload[4-7], signal level at payload[8]
    uint8_t iface = payload[1];
    uint8_t flags = payload[3];
    bool enabled     = flags & GATEWAY_IFACE_FLAG_ENABLED;
    bool initialized = flags & GATEWAY_IFACE_FLAG_INITIALIZED;
    bool linked      = flags & GATEWAY_IFACE_FLAG_LINKED;
    uint8_t ip[4];
    ip[0] = payload[4];
    ip[1] = payload[5];
    ip[2] = payload[6];
    ip[3] = payload[7];
    uint8_t signal_level = payload[8];

    const char *iface_name = iface == GATEWAY_IFACE_WIRED ? "Wired" :
                             iface == GATEWAY_IFACE_WIFI  ? "WiFi"  : "Unknown interface";
    ESP_LOGI(TAG, "%s Internet Gateway IP (%s) - %d.%d.%d.%d, signal level: %d, enabled: %s, initialized: %s, linked: %s",
             addr_info, iface_name, ip[0], ip[1], ip[2], ip[3], signal_level,
             enabled ? "yes" : "no", initialized ? "yes" : "no", linked ? "yes" : "no");

    // PROTOCOL.md 0x37 Network Config: only flag bits 0-2 have been seen
    if (flags & ~(GATEWAY_IFACE_FLAG_ENABLED | GATEWAY_IFACE_FLAG_INITIALIZED | GATEWAY_IFACE_FLAG_LINKED)) {
        record_undocumented(data, len);
    }

    // PROTOCOL.md 0x37 Network Config: only interfaces 0x00 and 0x01 are known
    if (iface >= MAX_GATEWAY_IFACES) {
        record_undocumented(data, len);
        return true;
    }

    // Update state only (no MQTT publishing)
    if (state_lock(ctx, "gateway IP")) {
        gateway_iface_t *gw = &ctx->pool_state->gateway_ifaces[iface];
        memcpy(gw->ip, ip, 4);
        gw->signal_level = signal_level;
        gw->enabled = enabled;
        gw->initialized = initialized;
        gw->linked = linked;
        gw->valid = true;
        state_unlock(ctx, true, NULL);
    }

    return true;
}

/**
 * Handler: Internet Gateway communications status (CMD 0x37 item 0x02)
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

    // Flag research-worthy anomalies (PROTOCOL.md 0x37 comms variant): a
    // status code absent from GATEWAY_COMMS_STATUS is a newly-seen value worth
    // capturing (the table notes "Add more status codes here as they are
    // discovered").
    if (strcmp(status_text, "Unknown") == 0) {
        record_undocumented(data, len);
    }

    // Update state only (no MQTT publishing)
    if (state_lock(ctx, "gateway comms status")) {
        ctx->pool_state->gateway_comms_status = comms_status;
        ctx->pool_state->gateway_comms_status_valid = true;
        state_unlock(ctx, true, NULL);
    }

    return true;
}

/**
 * Handler: Internet Gateway firmware version (CMD 0x37 item 0x05)
 * Payload {0x05, major, minor}. Only seen in reply to a CMD 0x36 request for
 * item 0x05. Firmware-version state is populated by the generic CMD 0x0A
 * handler (handle_firmware_version), so this handler is log-only.
 */
static bool handle_gateway_firmware_version(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    ESP_LOGI(TAG, "%s Internet Gateway firmware version - %d.%d", addr_info, payload[1], payload[2]);

    return true;
}

/**
 * Handler: Internet Gateway info (CMD 0x37) — routed by item ID.
 * payload[0] is the item, the same value as byte 10 of the CMD 0x36 request it
 * answers: 0x01 = network config, 0x02 = comms status, 0x04 = serial number,
 * 0x05 = firmware version. An unknown item is logged and recorded, and never
 * written into state.
 */
static bool handle_gateway_info(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    switch (payload[0]) {
        case 0x01: return handle_gateway_ip(data, len, payload, payload_len, addr_info, ctx);
        case 0x02: return handle_gateway_comms(data, len, payload, payload_len, addr_info, ctx);
        case 0x04: return handle_serial_number(data, len, payload, payload_len, addr_info, ctx);
        case 0x05: return handle_gateway_firmware_version(data, len, payload, payload_len, addr_info, ctx);
        default:
            ESP_LOGI(TAG, "%s Internet Gateway info - Unknown item (0x%02X)", addr_info, payload[0]);
            record_undocumented(data, len);
            return true;
    }
}

/**
 * Handler: Internet Gateway status broadcast (CMD 0x12, source 0x00F0)
 *
 * Payload (3 bytes): { major, minor, embedded_checksum }
 * where embedded_checksum == major + minor. The frame checksum at byte 13
 * is handled by the framing layer and is not part of the payload.
 *
 * Firmware-version state is populated by the generic CMD 0x0A handler
 * (handle_firmware_version), so this handler is log-only.
 */
static bool handle_gateway_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t major = payload[0];
    uint8_t minor = payload[1];
    uint8_t embedded_checksum = payload[2];
    uint8_t expected_checksum = (uint8_t)(major + minor);

    if (embedded_checksum == expected_checksum) {
        ESP_LOGI(TAG, "%s Internet Gateway status - firmware %d.%d (checksum 0x%02X OK)",
                 addr_info, major, minor, embedded_checksum);
    } else {
        ESP_LOGW(TAG, "%s Internet Gateway status - firmware %d.%d (checksum 0x%02X, expected 0x%02X)",
                 addr_info, major, minor, embedded_checksum, expected_checksum);
        record_undocumented(data, len);
    }

    return true;
}

/**
 * Handler: Register read request (CMD 0x39) — source-agnostic.
 *
 * Observed from both the Internet Gateway (`0x00F0`) and the Genus Heater
 * (`0x0070`); payload shape `{reg_id, slot_id}` is identical, only the source
 * address (and resulting checksum1 byte) differs. Dispatched in
 * dispatch_message() by CMD byte.
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
    // Unknown registers are not recorded here: the 0x38 reply carries the same
    // {reg, slot} plus the value, and is recorded as unhandled itself.
    if (!found) {
        snprintf(desc, sizeof(desc), "0x%02X/0x%02X", reg_id, slot_id);
    }

    ESP_LOGI(TAG, "%s Register read request - %s", addr_info, desc);

    // No state update needed - this is just a request message
    return true;
}

/**
 * Handler: Channel toggle command (CMD 0x10) — source-agnostic.
 * Sent by the Internet Gateway (0x00F0) for remote toggles and broadcast by
 * the Connect 8/10 controller (0x0062) when a channel button is pressed on
 * the controller itself.
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
    if (state_lock(ctx, "channel name lookup")) {
        if (channel_idx < MAX_CHANNELS && ctx->pool_state->channels[channel_idx].configured) {
            strncpy(channel_name, ctx->pool_state->channels[channel_idx].name, sizeof(channel_name) - 1);
        }
        state_unlock(ctx, false, NULL);
    }

    if (channel_name[0] != '\0') {
        ESP_LOGI(TAG, "%s Channel toggle command - Channel %d (%s)",
                 addr_info, channel_num, channel_name);
    } else {
        ESP_LOGI(TAG, "%s Channel toggle command - Channel %d (index 0x%02X, name unknown)",
                 addr_info, channel_num, channel_idx);
    }

    // No state update needed - this is a command message, not status
    // The controller will respond with an updated Channel Status message
    return true;
}

/**
 * Handler: Valve button (CMD 0x29) — source-agnostic.
 * Broadcast by the Connect 8/10 controller (0x0062) when a Valve button is
 * pressed on the controller itself; 1-byte 0-based valve index. Log-only —
 * the Touchscreen answers with a Valve State broadcast (CMD 0x27), which is
 * what updates state.
 */
static bool handle_valve_button(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t valve_idx = payload[0];
    uint8_t valve_num = valve_idx + 1;  // Convert to 1-based

    // Look up valve label from pool state
    char valve_name[32] = {0};
    if (state_lock(ctx, "valve name lookup")) {
        if (valve_idx < MAX_VALVE_SLOTS && ctx->pool_state->valves[valve_idx].configured) {
            strncpy(valve_name, ctx->pool_state->valves[valve_idx].name, sizeof(valve_name) - 1);
        }
        state_unlock(ctx, false, NULL);
    }

    if (valve_name[0] != '\0') {
        ESP_LOGI(TAG, "%s Valve button - Valve %d (%s)", addr_info, valve_num, valve_name);
    } else {
        ESP_LOGI(TAG, "%s Valve button - Valve %d", addr_info, valve_num);
    }

    // Only Valve 1 and Valve 2 (the two 0x27 slots) have been seen.
    if (valve_idx >= MAX_VALVE_SLOTS) {
        record_undocumented(data, len);
    }

    return true;
}

/**
 * Handler: Temperature setpoint command — Pool/Spa target (CMD 0x19, slot 0x01/0x02) — source-agnostic.
 * Used by the Internet Gateway (0x00F0) to set the Pool or Spa setpoint; the
 * temperature is repeated at payload[1] and payload[2]. The controller
 * responds with an updated Temperature Settings (CMD 0x17).
 * Dispatched in dispatch_message() by CMD byte.
 */
static bool handle_temp_set_cmd_pool_spa(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t target = payload[0];
    uint8_t temp_c = payload[1];  // Repeated at payload[2], only need one

    const char *target_name = (target == 0x01) ? "Pool" : "Spa";
    ESP_LOGI(TAG, "%s Temperature set command - %s setpoint -> %d°C",
             addr_info, target_name, temp_c);

    // No state update needed - the controller will broadcast the new setpoint
    return true;
}

/**
 * Handler: Temperature setpoint command — heater pair target (CMD 0x19, slot 0x03) — source-agnostic.
 * Used by the Touch Screen (0x0050) writing to the internal heater-setpoints
 * address 0x007F: 5-byte payload carrying both heater setpoints in °C and °F.
 *   payload[0] = 0x03  (heater-pair slot marker)
 *   payload[1] = Heater 2 setpoint °C
 *   payload[2] = Heater 1 setpoint °C
 *   payload[3] = Heater 2 setpoint °F
 *   payload[4] = Heater 1 setpoint °F
 * Byte order is reversed vs the 0x17 broadcast from 0x0070 (which is [H1, H2]).
 * The heater (0x0070) responds with an updated Genus Heater Temperature
 * Setting (CMD 0x17). Dispatched in dispatch_message() by CMD byte.
 */
static bool handle_temp_set_cmd_heaters(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 5) return false;

    uint8_t h2_c = payload[1];
    uint8_t h1_c = payload[2];
    uint8_t h2_f = payload[3];
    uint8_t h1_f = payload[4];

    ESP_LOGI(TAG, "%s Heater setpoint command - heater1=%d°C/%d°F, heater2=%d°C/%d°F",
             addr_info, h1_c, h1_f, h2_c, h2_f);

    // No state update needed - the heater will broadcast the new setpoints
    return true;
}

/**
 * Handler: Register write command (CMD 0x3A) — source-agnostic.
 * Sent by the Internet Gateway (0x00F0) for remote control and by the
 * Viron Chlorinator (0x0084), whose own app issues the same writes; same
 * {reg_id, slot, value} payload from either source. The Touchscreen (0x0050)
 * also uses it, addressed to the Gateway, to set the Gateway's WiFi SSID and
 * password (slot 0x07, string value).
 */
static bool handle_register_write_request(
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
    if (reg_id >= REG_ID_LIGHT_ZONE_STATE_0 && reg_id <= REG_ID_LIGHT_ZONE_STATE_7 && slot == 0x01) {
        // Light zone state control (REG_ID_LIGHT_ZONE_STATE_0–REG_ID_LIGHT_ZONE_STATE_7, slot 0x01)
        uint8_t zone_num = reg_id - REG_ID_LIGHT_ZONE_STATE_0 + 1;
        const char *state_name = (state == 0x00) ? "Off" : (state == 0x01) ? "Auto" : (state == 0x02) ? "On" : "Unknown";
        ESP_LOGI(TAG, "%s Light zone control command - Zone %d -> %s (0x%02X)",
                 addr_info, zone_num, state_name, state);
        if (state >= 0x03) {  // Only Off/Auto/On documented for light zone state
            record_undocumented(data, len);
        }
    } else if (reg_id >= REG_ID_LIGHT_ZONE_COLOR_0 && reg_id <= REG_ID_LIGHT_ZONE_COLOR_7 && slot == 0x01) {
        // Light zone color write (REG_ID_LIGHT_ZONE_COLOR_0–REG_ID_LIGHT_ZONE_COLOR_7, slot 0x01).
        // Logged as the raw code only: the observed write values (Viron
        // Chlorinator app) conflict with LIGHTING_COLOR_NAMES, so the color
        // value space appears install/brand-specific — see PROTOCOL.md 0x3A.
        uint8_t zone_num = reg_id - REG_ID_LIGHT_ZONE_COLOR_0 + 1;
        ESP_LOGI(TAG, "%s Light zone control command - Zone %d color -> 0x%02X",
                 addr_info, zone_num, state);
    } else if (reg_id == REG_ID_HEATER1_ONOFF && slot == 0x00) {
        // Heater 1 on/off control (REG_ID_HEATER1_ONOFF, slot 0x00)
        ESP_LOGI(TAG, "%s Heater 1 control command - Heater -> %s",
                 addr_info, state ? "On" : "Off");
        if (state >= 0x02) record_undocumented(data, len);  // documented: 0x00 Off, 0x01 On
    } else if (reg_id == REG_ID_HEATER2_ONOFF && slot == 0x00) {
        // Heater 2 on/off control (REG_ID_HEATER2_ONOFF, slot 0x00) — see PROTOCOL.md 0x3A
        ESP_LOGI(TAG, "%s Heater 2 control command - Heater -> %s",
                 addr_info, state ? "On" : "Off");
        if (state >= 0x02) record_undocumented(data, len);  // documented: 0x00 Off, 0x01 On
    } else if (reg_id == REG_ID_HEATER2_POOL_SETPOINT && slot == 0x00) {
        // Heater 2 pool setpoint write (REG_ID_HEATER2_POOL_SETPOINT, slot 0x00) — see PROTOCOL.md 0x3A
        ESP_LOGI(TAG, "%s Heater 2 pool setpoint command -> %d°C",
                 addr_info, state);
    } else if (reg_id == REG_ID_HEATER2_SPA_SETPOINT && slot == 0x00) {
        // Heater 2 spa setpoint write (REG_ID_HEATER2_SPA_SETPOINT, slot 0x00) — see PROTOCOL.md 0x3A
        ESP_LOGI(TAG, "%s Heater 2 spa setpoint command -> %d°C",
                 addr_info, state);
    } else if (reg_id == REG_ID_GATEWAY_WIFI_SSID && slot == 0x07) {
        // Internet Gateway WiFi SSID (slot 0x07), sent Touchscreen -> Gateway.
        // The value is an unterminated ASCII string filling the rest of the payload.
        ESP_LOGI(TAG, "%s Internet Gateway WiFi SSID -> \"%.*s\"",
                 addr_info, payload_len - 2, (const char *)&payload[2]);
    } else if (reg_id == REG_ID_GATEWAY_WIFI_PASSWORD && slot == 0x07) {
        // Internet Gateway WiFi password (slot 0x07) — masked, only its length is logged
        ESP_LOGI(TAG, "%s Internet Gateway WiFi password -> ******** (%d chars)",
                 addr_info, payload_len - 2);
    } else {
        // Recognised 0x3A frame, but this (register, slot) write target is not
        // in the documented set (PROTOCOL.md 0x3A).
        ESP_LOGW(TAG, "%s Register write command - Unknown Reg=0x%02X, Slot=0x%02X, State=0x%02X",
                 addr_info, reg_id, slot, state);
        record_undocumented(data, len);
    }

    // No state update needed - this is a command message, not status
    // The controller will respond with a register status update
    return true;
}

/**
 * Handler: Favourite control command (CMD 0x2A) — source-agnostic.
 * Sent to the Touchscreen by the Gateway (0x00F0) for remote activations
 * and by the Connect 8/10 controller (0x0062) for activations made at the
 * controller itself; same 1-byte favourite-value payload from either source.
 */
static bool handle_favourite_control_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t favourite_value = payload[0];
    const char *favourite_name;
    if (favourite_value == FAVOURITE_POOL)      favourite_name = "Pool";
    else if (favourite_value == FAVOURITE_SPA)  favourite_name = "Spa";
    else if (favourite_value == FAVOURITE_ALL_OFF)  favourite_name = "All Off";
    else if (favourite_value == FAVOURITE_ALL_AUTO) favourite_name = "All Auto";
    else if (favourite_value == FAVOURITE_NONE) favourite_name = "None";
    else if (favourite_value >= 0x02 && favourite_value <= 0x07) favourite_name = "Favourite";
    else favourite_name = "Unknown";

    ESP_LOGI(TAG, "%s Favourite control command - %s (0x%02X)",
             addr_info, favourite_name, favourite_value);

    // Documented values: 0x00-0x07 (Pool/Spa/Fav1-6), All Off, All Auto.
    if (favourite_value > 0x07
         && favourite_value != FAVOURITE_ALL_OFF
         && favourite_value != FAVOURITE_ALL_AUTO
         && favourite_value != FAVOURITE_NONE) {
        record_undocumented(data, len);
    }

    pool_state_t state_snapshot;
    bool should_publish = false;

    if (state_lock(ctx, "favourite control command")) {
        ctx->pool_state->active_favourite = favourite_value;
        ctx->pool_state->active_favourite_valid = true;
        should_publish = true;
        state_unlock(ctx, false, &state_snapshot);
    }

    if (should_publish) {
        mqtt_publish_favourite(&state_snapshot);
    }
    return true;
}

/**
 * Handler: Chlorine output level (CMD 0x1D, slot 0x00) from VX 11S v3 (0x0081).
 * Payload: {0x00, level, 0x00} where level is an integer 1–8.
 * Per manual: applies to Pool mode only; Spa mode always outputs at level 1.
 */
static bool handle_chlor_output_level(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t level = payload[1];
    ESP_LOGI(TAG, "%s Chlorine output level - %d", addr_info, level);

    bool undocumented = false;
    if (level == 0 || level > 8) {
        ESP_LOGW(TAG, "%s Chlorine output level out of expected range 1-8: %d", addr_info, level);
        undocumented = true;
    }
    // PROTOCOL.md 0x1D slot 0x00: byte 12 is observed as 0x00 in normal
    // operation but may carry LOW SALT / NO FLOW warning bits — capture any
    // non-zero value for verification.
    if (payload_len >= 3 && payload[2] != 0x00) {
        ESP_LOGW(TAG, "%s Chlorine output level - byte 12 non-zero (possible warning flags): 0x%02X",
                 addr_info, payload[2]);
        undocumented = true;
    }
    if (undocumented) {
        record_undocumented(data, len);
    }

    pool_state_t snapshot;
    if (!state_lock(ctx, "chlorine output level")) return true;
    ctx->pool_state->chlor_output_level = level;
    ctx->pool_state->chlor_output_level_valid = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator pH setpoint (CMD 0x1D, channel 0x01) — source-agnostic.
 * Same `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
 * 0x0084 Viron chlorinators; dispatched in dispatch_message() by CMD byte.
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
    if (!state_lock(ctx, "pH setpoint")) return true;
    ctx->pool_state->ph_setpoint = value;
    ctx->pool_state->ph_setpoint_valid = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator ORP setpoint (CMD 0x1D, channel 0x02) — source-agnostic.
 * Same `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
 * 0x0084 Viron chlorinators; dispatched in dispatch_message() by CMD byte.
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
    if (!state_lock(ctx, "ORP setpoint")) return true;
    ctx->pool_state->orp_setpoint = value;
    ctx->pool_state->orp_setpoint_valid = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator pH reading (CMD 0x1F, channel 0x01) — source-agnostic.
 * Same `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
 * 0x0084 Viron chlorinators; dispatched in dispatch_message() by CMD byte.
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
    if (!state_lock(ctx, "pH reading")) return true;
    ctx->pool_state->ph_reading = value;
    ctx->pool_state->ph_valid = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator ORP reading (CMD 0x1F, channel 0x02) — source-agnostic.
 * Same `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
 * 0x0084 Viron chlorinators; dispatched in dispatch_message() by CMD byte.
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
    if (!state_lock(ctx, "ORP reading")) return true;
    ctx->pool_state->orp_reading = value;
    ctx->pool_state->orp_valid = true;
    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator status broadcast (CMD 0x12, source 0x0090 RolaChem or
 * 0x0084 Viron)
 *
 * 1-byte payload: configured operating mode. Mapping is tentative — see §32.
 */
static bool handle_chlor_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t mode = payload[0];
    const char *mode_name;
    switch (mode) {
        case 0x00: mode_name = "Off";  break;
        case 0x01: mode_name = "Auto"; break;
        case 0x02: mode_name = "On";   break;
        default:   mode_name = "Unknown"; break;
    }

    ESP_LOGI(TAG, "%s Chlorinator status - mode=%s (0x%02X)", addr_info, mode_name, mode);

    // Documented modes are Off/Auto/On (0x00-0x02); flag anything else.
    if (mode > 0x02) {
        record_undocumented(data, len);
    }

    if (state_lock(ctx, "chlorinator mode")) {
        ctx->pool_state->chlor_mode = mode;
        ctx->pool_state->chlor_mode_valid = true;
        state_unlock(ctx, true, NULL);
    }

    return true;
}

/**
 * Handler: VX 11S v3 Chlorinator status broadcast (CMD 0x12, source 0x0081)
 *
 * 1-byte payload: Always 0x00 observed. Mapping unknown.
 */
static bool handle_vx11s_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t data_byte = payload[0];

    if (data_byte != 0x00) {
        ESP_LOGI(TAG, "%s VX 11S v3 Salt Chlorinator status - UNEXPECTED VALUE: payload=0x%02X (expected 0x00)", addr_info, data_byte);
        record_undocumented(data, len);
    } else {
        ESP_LOGI(TAG, "%s VX 11S v3 Salt Chlorinator status: 0x%02X", addr_info, data_byte);
    }

    return true;
}

/**
 * Handler: Set channel state unicast (CMD 0x0F) — source-agnostic.
 * Log-only — no pool_state update since the Touchscreen applies the state and
 * broadcasts it back via CMD 0x0B, which is what we record.
 *
 * 2-byte payload: {1-based channel index, target state}. The state uses the
 * CMD 0x0B Channel State code space (CHANNEL_STATE_NAMES), extended pump
 * speeds included. Only pump-driven channels act on it; see PROTOCOL.md
 * command `0x0F`.
 *
 * The Touchscreen does not validate the source address at all: 0x0084
 * (Viron) and 0x0081 (VX 11S v3) are confirmed chlorinator sources on the
 * bus, the Connect 8/10 (0x0062) sends it for its Cleaning button, and this
 * firmware originates it as its own 0xAC1D.
 */
static bool handle_set_channel_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len != 2) return false;

    uint8_t channel_num = payload[0];   // 1-based
    uint8_t state = payload[1];

    if (channel_num < 1 || channel_num > MAX_CHANNELS || state >= CHANNEL_STATE_COUNT) {
        ESP_LOGW(TAG, "%s Set channel state - UNEXPECTED VALUE: channel=0x%02X state=0x%02X (expected channel 0x01-0x%02X, state 0x00-0x%02X)",
                 addr_info, channel_num, state, MAX_CHANNELS, CHANNEL_STATE_COUNT - 1);
        record_undocumented(data, len);
        return true;
    }

    ESP_LOGI(TAG, "%s Set channel state - channel %u -> %s (0x%02X)",
             addr_info, channel_num, CHANNEL_STATE_NAMES[state], state);

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

    if (zone_idx >= MAX_LIGHT_ZONES) {  // PROTOCOL.md 0x06: zones 1-8 map to index 0-7
        ESP_LOGW(TAG, "%s Lighting zone config - zone index out of range 0-%d: %d",
                 addr_info, MAX_LIGHT_ZONES - 1, zone_idx);
        record_undocumented(data, len);
        return true;
    }

    if (light_on > 0x01) {  // documented status values: 0x00 off, 0x01 on
        ESP_LOGW(TAG, "%s Lighting zone %d - undocumented status byte: 0x%02X",
                 addr_info, zone_idx + 1, light_on);
        record_undocumented(data, len);
    }

    ESP_LOGI(TAG, "%s Lighting zone %d - %s", addr_info, zone_idx + 1, light_on ? "On" : "Off");

    pool_state_t state_snapshot;
    bool newly_configured = false;

    if (!state_lock(ctx, "light config")) return true;
    newly_configured = !ctx->pool_state->lighting[zone_idx].configured;
    ctx->pool_state->lighting[zone_idx].zone       = zone_idx + 1;
    ctx->pool_state->lighting[zone_idx].configured = true;
    ctx->pool_state->lighting[zone_idx].active     = (light_on != 0);
    state_unlock(ctx, true, &state_snapshot);

    if (newly_configured) {
        register_requester_notify();
    }

    if (ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_idx + 1);
    }

    return true;
}

/**
 * Handler: Lighting zone color broadcast (CMD 0x07) — log-only
 * Dispatched on the CMD byte alone (source-agnostic).
 *
 * Color companion to the CMD 0x06 light config broadcast; only emitted for
 * multicolor-capable zones. The color byte mirrors the zone's Light Zone
 * Color register (0xD0+zone, slot 0x01), which is the state source that
 * updates pool_state — so this handler just logs.
 */
static bool handle_light_color(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t zone_idx = payload[0];
    uint8_t color    = payload[1];

    if (zone_idx >= MAX_LIGHT_ZONES) {  // PROTOCOL.md 0x07: zones 1-8 map to index 0-7
        ESP_LOGW(TAG, "%s Lighting zone color - zone index out of range 0-%d: %d",
                 addr_info, MAX_LIGHT_ZONES - 1, zone_idx);
        record_undocumented(data, len);
        return true;
    }

    ESP_LOGI(TAG, "%s Lighting zone %d color - 0x%02X", addr_info, zone_idx + 1, color);

    return true;
}

/**
 * Handler: Light resync command (CMD 0x3C) — log-only
 * Dispatched on the CMD byte alone (source-agnostic).
 *
 * Resyncs a light zone's light; observed during light configuration
 * and color operations. What the resync does at the light hardware is not
 * yet confirmed, so this handler just logs.
 */
static bool handle_light_resync(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t zone_idx = payload[0];

    if (zone_idx >= MAX_LIGHT_ZONES) {  // PROTOCOL.md 0x3C: zones 1-8 map to index 0-7
        ESP_LOGW(TAG, "%s Light resync - zone index out of range 0-%d: %d",
                 addr_info, MAX_LIGHT_ZONES - 1, zone_idx);
        record_undocumented(data, len);
        return true;
    }

    ESP_LOGI(TAG, "%s Light zone %d resync", addr_info, zone_idx + 1);

    return true;
}

/**
 * Handler: Pre-valve-command frame (CMD 0x1A) — log-only
 *
 * Zero-payload unicast from the Touchscreen (0x0050) to Internal Control
 * (0x007F), sent ~120 ms before every CMD 0x41 valve actuator command.
 * Purpose is unknown — no reply from 0x007F has ever been observed.
 * Logged only so it stops being reported as unknown.
 */
static bool handle_pre_valve_frame(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    ESP_LOGI(TAG, "%s Pre-valve-command frame", addr_info);

    // No payload check here, deliberately: a 0x1A that matches the pattern is
    // byte-for-byte identical every time. The pattern pins bytes 0-9, framing
    // slices the frame at the LEN byte (which the pattern pins to 0x0B) and
    // requires the last byte to be ETX, so there is no byte left to vary. Any
    // 0x1A that differs fails the pattern and is recorded as UNHANDLED by
    // decode_message instead.
    return true;
}

/**
 * Handler: Valve actuator command (CMD 0x41) — log-only
 *
 * Unicast from the Touchscreen (0x0050) to Internal Control (0x007F) that
 * drives a group of motorised valve actuators to one of their two endpoints on
 * a mode change. The position byte names an endpoint, not a flow path: a
 * three-position toggle on the actuator body reverses which way it travels for
 * a given command, so identical payloads plumb differently across installs.
 */
static bool handle_valve_actuator_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t position = payload[0];
    uint8_t group    = payload[1];

    ESP_LOGI(TAG, "%s Valve actuator command - group %d, position %d", addr_info, group, position);

    // Flag anything not yet observed for the unknown-messages page. The
    // actuators have two cam-limited endpoints and only positions 0x00/0x01
    // have been seen. Group is only ever 0x01, and is checked against that
    // rather than an upper bound: the controller has four actuator sockets
    // but the pool/spa pair is ganged, so at most three groups could exist -
    // and whether the two auxiliary sockets are addressable here at all is
    // unknown. A frame naming any other group is the observation that would
    // settle it, so it should not pass silently.
    if ((group != 0x01) || (position > 1)) {
        record_undocumented(data, len);
    }

    return true;
}

/**
 * Handler: Solar setpoint broadcast (CMD 0x2D) — log-only
 * Dispatched on the CMD byte alone (source-agnostic).
 *
 * Broadcast by the Touchscreen when the solar setpoint is changed; carries the
 * setpoint as a single °C byte. The same value lives in the solar setpoint
 * register (0x3A/slot 0x01, handle_solar_setpoint).
 */
static bool handle_solar_setpoint_broadcast(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    ESP_LOGI(TAG, "%s Solar setpoint broadcast - %d°C", addr_info, payload[0]);

    return true;
}

/**
 * Apply retained Solar mode (CMD 0x2C) and output (0x0062 CMD 0x12) to a channel
 * (type 0xFC). Called wherever a channel's state may be written from another
 * source (CMD 0x0B, the channel type and channel state registers), so stale
 * ordinary-channel data cannot overwrite either independently observed value.
 * Caller must hold the state mutex. Returns true if the channel changed.
 */
static bool apply_solar_status(pool_state_t *ps, channel_state_t *ch)
{
    if (ch->type != CHANNEL_TYPE_SOLAR_CONTROLLER) return false;
    bool changed = false;
    if (ps->solar_mode_valid && ch->state != ps->solar_mode) {
        ch->state = ps->solar_mode;
        changed = true;
    }
    if (ps->solar_active_valid && ch->active != ps->solar_active) {
        ch->active = ps->solar_active;
        changed = true;
    }
    return changed;
}

/**
 * Handler: Solar status broadcast (CMD 0x2C)
 * Dispatched on the CMD byte alone (source-agnostic).
 *
 * Solar config/status block from the Touchscreen: config bitmask (season,
 * flush daily, filter pump required), selected mode, unknown configuration
 * bytes and the temperature differential. Only the mode here is authoritative
 * for a dedicated Solar channel (type 0xFC). Output activity comes from
 * 0x0062 CMD 0x12 payload[1] bit 2, independently of selected mode: its CMD
 * 0x0B tuple remains FC 00 00 even when Solar is Auto and running, and CMD
 * 0x0D omits that controller-managed output.
 */
static bool handle_solar_status_broadcast(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 6) return false;

    uint8_t flags = payload[0];
    uint8_t mode  = payload[1];
    const char *mode_name = (mode == 0x00) ? "Off" : (mode == 0x01) ? "Auto" : (mode == 0x02) ? "On" : "Unknown";

    ESP_LOGI(TAG, "%s Solar status - Mode: %s, Season: %s, Flush daily: %s, "
             "Filter pump required: %s, Unknown config: 0x%02X, Differential: %d°C, Unknown: 0x%02X",
             addr_info, mode_name,
             (flags & 0x20) ? "Summer" : "Winter",
             (flags & 0x02) ? "on" : "off",
             (flags & 0x08) ? "yes" : "no",
             payload[2],
             payload[5], payload[3]);

    // Byte 12 stays 0x02 even with mode Off and the pump physically stopped.
    // Its meaning is unresolved; do not use it to create an activity sample.
    bool mode_known = mode <= 0x02;
    if (!mode_known) {
        ESP_LOGW(TAG, "%s Solar status has undocumented mode 0x%02X", addr_info, mode);
    }
    if (payload[2] > 0x02) {
        ESP_LOGW(TAG, "%s Solar status has undocumented configuration byte 0x%02X",
                 addr_info, payload[2]);
    }
    if (!mode_known || payload[2] > 0x02) {
        record_undocumented(data, len);
    }

    pool_state_t state_snapshot;
    uint8_t channels_to_publish[MAX_CHANNELS] = {0};
    int num_to_publish = 0;

    if (state_lock(ctx, "solar status")) {
        pool_state_t *ps = ctx->pool_state;
        if (mode_known) {
            ps->solar_mode = mode;
            ps->solar_mode_valid = true;
        }

        for (int i = 0; i < MAX_CHANNELS; i++) {
            channel_state_t *channel = &ps->channels[i];
            if (channel->configured && apply_solar_status(ps, channel)) {
                channels_to_publish[num_to_publish++] = (uint8_t)(i + 1);
            }
        }
        // Only take the (large) snapshot when there is something to publish
        state_unlock(ctx, true, num_to_publish > 0 ? &state_snapshot : NULL);
    }

    if (num_to_publish > 0 && ctx->enable_mqtt) {
        for (int i = 0; i < num_to_publish; i++) {
            mqtt_publish_channel(&state_snapshot, channels_to_publish[i]);
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

    uint8_t timer_num = reg_id - REG_ID_TIMER_0 + 1;

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
        record_undocumented(data, len);
    }

    // PROTOCOL.md 0x38 timers: only day bits 0-6 (Mon-Sun) are documented.
    if (days & 0x80) {
        ESP_LOGW(TAG, "%s Timer %d - undocumented day bit 7 set (days=0x%02X)", addr_info, timer_num, days);
        record_undocumented(data, len);
    }

    // Update state
    if (timer_num >= 1 && timer_num <= MAX_TIMERS) {
        if (state_lock(ctx, "timer")) {
            int idx = timer_num - 1;
            ctx->pool_state->timers[idx].timer_num    = timer_num;
            ctx->pool_state->timers[idx].start_hour   = start_hour;
            ctx->pool_state->timers[idx].start_minute = start_minute;
            ctx->pool_state->timers[idx].stop_hour    = stop_hour;
            ctx->pool_state->timers[idx].stop_minute  = stop_minute;
            ctx->pool_state->timers[idx].days         = days;
            ctx->pool_state->timers[idx].valid        = true;
            state_unlock(ctx, true, NULL);
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
    channel_type_t ch_type = (channel_type_t)payload[2];
    uint8_t ch_num = reg_id - REG_ID_CHANNEL_TYPE_0 + 1;

    const char *type_name = get_channel_type_name(ch_type);
    ESP_LOGI(TAG, "%s Channel %d type - %s (%d)", addr_info, ch_num, type_name, ch_type);

    if (!channel_type_is_known(ch_type)) {  // not in PROTOCOL.md 0x0B channel-type set
        record_undocumented(data, len);
    }

    if (ch_type != CHANNEL_TYPE_UNUSED) {
        // Update pool state
        if (state_lock(ctx, "channel type")) {
            ctx->pool_state->channels[ch_num - 1].type = ch_type;
            ctx->pool_state->channels[ch_num - 1].configured = true;
            apply_solar_status(ctx->pool_state, &ctx->pool_state->channels[ch_num - 1]);
            state_unlock(ctx, true, NULL);
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
    uint8_t ch_num = reg_id - REG_ID_CHANNEL_NAME_0 + 1;

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
        if (state_lock(ctx, "channel name")) {
            if (ch_num <= MAX_CHANNELS) {
                strncpy(ctx->pool_state->channels[ch_num - 1].name, name, sizeof(ctx->pool_state->channels[ch_num - 1].name) - 1);
                ctx->pool_state->channels[ch_num - 1].id = ch_num;
            }
            state_unlock(ctx, ch_num <= MAX_CHANNELS, NULL);
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
    uint8_t ch_num = reg_id - REG_ID_CHANNEL_STATE_0 + 1;

    const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
    ESP_LOGI(TAG, "%s Channel %d state - %s", addr_info, ch_num, state_name);

    if (ch_num > MAX_CHANNELS || state >= CHANNEL_STATE_COUNT) {
        record_undocumented(data, len);
    }

    if (ch_num > MAX_CHANNELS) return true;

    pool_state_t state_snapshot;
    bool changed = false;
    if (state_lock(ctx, "channel state")) {
        channel_state_t *ch = &ctx->pool_state->channels[ch_num - 1];
        uint8_t prev_state = ch->state;
        // State registers are broadcast for unused channels too (e.g. in the
        // periodic register dump) — only a known channel type marks it in use
        if (ch->type != CHANNEL_TYPE_UNUSED && (!ch->configured || ch->state != state)) {
            ch->state = state;
            // A dedicated Solar channel's state register is as stale as its
            // CMD 0x0B tuple — CMD 0x2C's mode wins once it has been seen
            apply_solar_status(ctx->pool_state, ch);
            changed = !ch->configured || ch->state != prev_state;
            ch->configured = true;
        }
        state_unlock(ctx, changed, &state_snapshot);
    }

    if (changed && ctx->enable_mqtt) {
        mqtt_publish_channel(&state_snapshot, ch_num);
    }

    return true;
}

/**
 * Get channel category name from category code.
 *
 * If the code is known, returns the static label. Otherwise formats
 * "Unknown 0xXX" into the caller-supplied buffer and returns a pointer to it.
 */
static const char* get_channel_category_name(uint8_t code, char *fallback_buf, size_t buf_size) {
    switch (code) {
        case CHANNEL_CATEGORY_POOL_EQUIPMENT: return "Pool equipment";
        case CHANNEL_CATEGORY_LIGHT:          return "Light";
        case CHANNEL_CATEGORY_HEATER_POWER:   return "Controlled Heater Power";
        case CHANNEL_CATEGORY_SECONDARY_HEATER: return "Secondary Heater";
    }
    snprintf(fallback_buf, buf_size, "Unknown 0x%02X", code);
    return fallback_buf;
}

/**
 * Handler: Channel category
 * Register range: 0xF5-0xFC, Slot: 0x01
 * Values: 0x01=Pool equipment, 0x02=Light, 0x03=Controlled Heater Power,
 * 0x05=Secondary Heater (0x04 not observed)
 * Only broadcast for channels that are in use.
 */
static bool handle_channel_category(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t category = payload[2];
    uint8_t ch_num = reg_id - REG_ID_CHANNEL_CATEGORY_0 + 1;

    char fallback[16];
    const char *category_name = get_channel_category_name(category, fallback, sizeof(fallback));
    ESP_LOGI(TAG, "%s Channel %d category - %s (%d)", addr_info, ch_num, category_name, category);

    if (category != CHANNEL_CATEGORY_POOL_EQUIPMENT && category != CHANNEL_CATEGORY_LIGHT &&
        category != CHANNEL_CATEGORY_HEATER_POWER && category != CHANNEL_CATEGORY_SECONDARY_HEATER) {
        record_undocumented(data, len);
    }

    if (state_lock(ctx, "channel category")) {
        ctx->pool_state->channels[ch_num - 1].category = category;
        ctx->pool_state->channels[ch_num - 1].configured = true;
        state_unlock(ctx, true, NULL);
    }

    return true;
}

/**
 * Handler: Lighting zone enabled flag
 * Register range: 0x90-0x97, Slot: 0x01
 *
 * 0x01 = zone is configured in the controller, 0x00 = not configured.
 * Enabled zones are rebroadcast regularly; disabled zones only appear at
 * startup or after a configuration change.
 */
static bool handle_light_zone_enabled(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t enabled = payload[2];
    uint8_t zone_idx = reg_id - REG_ID_LIGHT_ZONE_ENABLED_0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone enabled: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        record_undocumented(data, len);
        return true;  // captured as undocumented; don't fall through to UNHANDLED
    }

    if (enabled >= 0x02) {
        ESP_LOGW(TAG, "%s Lighting zone enabled out of expected range 0x00-0x01: 0x%02X", addr_info, enabled);
        record_undocumented(data, len);
    }

    ESP_LOGI(TAG, "%s Lighting zone %d enabled - %s", addr_info, zone_idx + 1, enabled ? "Yes" : "No");

    bool newly_configured = false;
    bool should_publish = false;
    pool_state_t state_snapshot;

    if (state_lock(ctx, "light zone enabled")) {
        newly_configured = enabled && !ctx->pool_state->lighting[zone_idx].configured;
        ctx->pool_state->lighting[zone_idx].zone       = zone_idx + 1;
        ctx->pool_state->lighting[zone_idx].configured = (enabled != 0);

        should_publish = (enabled != 0);
        state_unlock(ctx, true, &state_snapshot);
    }

    if (newly_configured) {
        register_requester_notify();
    }

    if (should_publish && ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_idx + 1);
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
    uint8_t zone_idx = reg_id - REG_ID_LIGHT_ZONE_STATE_0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone state: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        record_undocumented(data, len);
        return true;  // captured as undocumented; don't fall through to UNHANDLED
    }

    const char *state_name = (state < LIGHTING_STATE_COUNT) ? LIGHTING_STATE_NAMES[state] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d state - %s", addr_info, zone_idx + 1, state_name);

    if (state >= LIGHTING_STATE_COUNT) {  // documented states are Off/Auto/On
        record_undocumented(data, len);
    }

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (state_lock(ctx, "light zone state")) {
        ctx->pool_state->lighting[zone_idx].zone = zone_idx + 1;
        ctx->pool_state->lighting[zone_idx].state = state;

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_unlock(ctx, true, &state_snapshot);
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
    uint8_t zone_idx = reg_id - REG_ID_LIGHT_ZONE_COLOR_0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone color: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        record_undocumented(data, len);
        return true;  // captured as undocumented; don't fall through to UNHANDLED
    }

    const char *color_name = (color < LIGHTING_COLOR_COUNT) ? LIGHTING_COLOR_NAMES[color] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d color - %s (%d)", addr_info, zone_idx + 1, color_name, color);

    if (color >= LIGHTING_COLOR_COUNT) {  // not in the known colour table
        record_undocumented(data, len);
    }

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (state_lock(ctx, "light zone color")) {
        ctx->pool_state->lighting[zone_idx].color = color;

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_unlock(ctx, true, &state_snapshot);
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
    uint8_t zone_idx = reg_id - REG_ID_LIGHT_ZONE_MULTICOLOR_0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone multicolor: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        record_undocumented(data, len);
        return true;  // captured as undocumented; don't fall through to UNHANDLED
    }

    ESP_LOGI(TAG, "%s Lighting zone %d multicolor - %s", addr_info, zone_idx + 1, capable ? "Yes" : "No");

    pool_state_t state_snapshot;
    bool should_publish = false;

    if (state_lock(ctx, "light zone multicolor")) {
        ctx->pool_state->lighting[zone_idx].multicolor = (capable != 0);
        ctx->pool_state->lighting[zone_idx].multicolor_valid = true;
        should_publish = ctx->pool_state->lighting[zone_idx].configured;
        state_unlock(ctx, true, &state_snapshot);
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
    uint8_t zone_idx = reg_id - REG_ID_LIGHT_ZONE_NAME_0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone name: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        record_undocumented(data, len);
        return true;  // captured as undocumented; don't fall through to UNHANDLED
    }

    const char *name = (name_id < LIGHT_ZONE_NAME_COUNT) ? LIGHT_ZONE_NAME_TABLE[name_id] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d name - %s (%d)", addr_info, zone_idx + 1, name, name_id);

    if (name_id >= LIGHT_ZONE_NAME_COUNT) {  // not in the known name-code table
        record_undocumented(data, len);
    }

    pool_state_t state_snapshot;
    bool should_publish = false;

    if (state_lock(ctx, "light zone name")) {
        ctx->pool_state->lighting[zone_idx].name_id = name_id;
        ctx->pool_state->lighting[zone_idx].name_valid = true;
        should_publish = ctx->pool_state->lighting[zone_idx].configured;
        state_unlock(ctx, true, &state_snapshot);
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
    uint8_t zone_idx = reg_id - REG_ID_LIGHT_ZONE_ACTIVE_0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone active: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        record_undocumented(data, len);
        return true;  // captured as undocumented; don't fall through to UNHANDLED
    }

    if (active >= 0x02) {
        ESP_LOGW(TAG, "%s Lighting zone active out of expected range 0x00-0x01: 0x%02X", addr_info, active);
        record_undocumented(data, len);
    }

    ESP_LOGI(TAG, "%s Lighting zone %d active - %s", addr_info, zone_idx + 1, active ? "Yes" : "No");

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (state_lock(ctx, "light zone active")) {
        ctx->pool_state->lighting[zone_idx].active = (active != 0);

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_unlock(ctx, true, &state_snapshot);
    }

    if (should_publish && ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Valve labels
 * Register range: 0xD0-0xD7, Slot: 0x02
 *
 * Only Valves 1-2 (the two CMD 0x27 slots) are stored; 0xD2-0xD7 have only
 * ever been seen empty, so they are logged and a non-empty one is flagged.
 */
static bool handle_valve_label(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t zone_num = reg_id - REG_ID_VALVE_LABEL_0 + 1;

    // Safely copy string from payload — protocol does not guarantee null termination
    char label[32] = {0};
    int str_len = payload_len - 2;
    if (str_len > (int)sizeof(label) - 1) str_len = (int)sizeof(label) - 1;
    memcpy(label, &payload[2], str_len);

    ESP_LOGI(TAG, "%s Valve zone %d label (0x%02X) - \"%s\"", addr_info, zone_num, reg_id, label);

    if (zone_num > MAX_VALVE_SLOTS) {
        if (label[0] != '\0') {
            ESP_LOGW(TAG, "%s Valve zone %d label - beyond the %d supported valve slots",
                     addr_info, zone_num, MAX_VALVE_SLOTS);
            record_undocumented(data, len);
        }
        return true;
    }

    // Update pool state
    pool_state_t state_snapshot;
    if (!state_lock(ctx, "valve label")) return true;
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
    }

    // Also store directly in valve state for MQTT name-change detection
    int valve_idx = zone_num - 1;
    strncpy(ctx->pool_state->valves[valve_idx].name, label,
            sizeof(ctx->pool_state->valves[valve_idx].name) - 1);
    ctx->pool_state->valves[valve_idx].name[sizeof(ctx->pool_state->valves[valve_idx].name) - 1] = '\0';

    state_unlock(ctx, slot >= 0, &state_snapshot);

    // Re-publish valve discovery and state now that the name is known
    if (ctx->enable_mqtt) {
        mqtt_publish_valve(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Timer configured flag
 * Register range: 0xE8-0xF7, Slot: 0x03 (0x01 = timer configured, 0x00 = not).
 * Log-only — the timer's own schedule registers (0x08-0x17, slot 0x04) are what
 * update pool_state->timers[].
 */
static bool handle_timer_configured(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t configured = payload[2];
    uint8_t timer_num = reg_id - REG_ID_TIMER_CONFIGURED_0 + 1;

    if (configured >= 0x02) {
        ESP_LOGW(TAG, "%s Timer configured out of expected range 0x00-0x01: 0x%02X", addr_info, configured);
        record_undocumented(data, len);
    }

    ESP_LOGI(TAG, "%s Timer %d configured - %s", addr_info,
             timer_num, configured ? "Yes" : "No");

    return true;
}

/**
 * Handler: Valve configured flag
 * Register range: 0x26-0x2D, Slot: 0x02 (0x01 = valve zone configured, 0x00 = not).
 * Log-only — the configured flag in the CMD 0x27 Valve State Broadcast is what
 * updates pool_state->valves[].
 */
static bool handle_valve_configured(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t configured = payload[2];
    uint8_t zone_num = reg_id - REG_ID_VALVE_CONFIGURED_0 + 1;

    if (configured >= 0x02) {
        ESP_LOGW(TAG, "%s Valve configured out of expected range 0x00-0x01: 0x%02X", addr_info, configured);
        record_undocumented(data, len);
    } else if (configured && zone_num > MAX_VALVE_SLOTS) {
        // Valves 3-8 have only ever read 0x00 (CMD 0x27 carries two slots)
        ESP_LOGW(TAG, "%s Valve zone %d configured - beyond the %d supported valve slots",
                 addr_info, zone_num, MAX_VALVE_SLOTS);
        record_undocumented(data, len);
    }

    ESP_LOGI(TAG, "%s Valve zone %d configured - %s", addr_info,
             zone_num, configured ? "Yes" : "No");

    return true;
}

/**
 * Handler: Valve state register
 * Register range: 0xE0-0xE7, Slot: 0x02 (0x00 = Off, 0x01 = Auto, 0x02 = On),
 * the same value as the state byte of the valve's slot in CMD 0x27.
 * Log-only — the CMD 0x27 Valve State Broadcast is what updates
 * pool_state->valves[].
 */
static bool handle_valve_state_register(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t state = payload[2];
    uint8_t zone_num = reg_id - REG_ID_VALVE_STATE_0 + 1;

    if (state > 0x02) {
        ESP_LOGW(TAG, "%s Valve state out of expected range 0x00-0x02: 0x%02X", addr_info, state);
        record_undocumented(data, len);
    } else if (state && zone_num > MAX_VALVE_SLOTS) {
        // Valves 3-8 have only ever read 0x00 (CMD 0x27 carries two slots)
        ESP_LOGW(TAG, "%s Valve zone %d state - beyond the %d supported valve slots",
                 addr_info, zone_num, MAX_VALVE_SLOTS);
        record_undocumented(data, len);
    }

    const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
    ESP_LOGI(TAG, "%s Valve zone %d state - %s", addr_info, zone_num, state_name);

    return true;
}

/**
 * Handler: Favourite label
 * Register range: 0x31–0x38, Slot: 0x03
 * Index 0=Pool, 1=Spa, 2–7=Favourites 1–6
 */
static bool handle_favourite_label(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    int index = reg_id - REG_ID_FAVOURITE_LABEL_0;
    if (index < 0 || index >= MAX_FAVOURITES) return false;

    char label[32] = {0};
    int str_len = payload_len - 2;
    if (str_len > (int)sizeof(label) - 1) str_len = (int)sizeof(label) - 1;
    memcpy(label, &payload[2], str_len);

    ESP_LOGI(TAG, "%s Favourite %d label (0x%02X) - \"%s\"", addr_info, index, reg_id, label);

    pool_state_t state_snapshot;
    if (!state_lock(ctx, "favourite label")) return true;
    strncpy(ctx->pool_state->favourites[index].name, label,
            sizeof(ctx->pool_state->favourites[index].name) - 1);
    ctx->pool_state->favourites[index].name[sizeof(ctx->pool_state->favourites[index].name) - 1] = '\0';
    ctx->pool_state->favourites[index].name_valid = true;
    state_unlock(ctx, true, &state_snapshot);

    mqtt_publish_favourite(&state_snapshot);
    return true;
}

/**
 * Handler: Favourite enable flag

 * Register range: 0x21–0x28, Slot: 0x03
 * Index 0=Pool, 1=Spa, 2–7=Favourites 1–6
 */
/**
 * Handler: Active favourite (register 0x20, slot 0x03)
 * Value is the CMD 0x2A favourite value (0x00=Pool, 0x01=Spa,
 * 0x02-0x07=Fav1-6); 0xFF = no favourite active. Broadcast on change and in
 * the periodic register dump.
 */
static bool handle_active_favourite(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t value = payload[2];

    const char *name;
    if (value == FAVOURITE_POOL)                       name = "Pool";
    else if (value == FAVOURITE_SPA)                   name = "Spa";
    else if (value >= 0x02 && value < MAX_FAVOURITES)  name = "Favourite";
    else if (value == FAVOURITE_ALL_OFF)               name = "All Off";
    else if (value == FAVOURITE_ALL_AUTO)              name = "All Auto";
    else if (value == FAVOURITE_NONE)                  name = "None";
    else                                               name = "Unknown";

    ESP_LOGI(TAG, "%s Active favourite - %s (0x%02X)", addr_info, name, value);

    if (value >= MAX_FAVOURITES && value != FAVOURITE_ALL_OFF && value != FAVOURITE_ALL_AUTO &&
        value != FAVOURITE_NONE) {
        record_undocumented(data, len);
    }

    pool_state_t state_snapshot;
    bool should_publish = false;
    if (state_lock(ctx, "active favourite")) {
        ctx->pool_state->active_favourite = value;
        ctx->pool_state->active_favourite_valid = true;
        should_publish = true;
        state_unlock(ctx, true, &state_snapshot);
    }

    if (should_publish) {
        mqtt_publish_favourite(&state_snapshot);
    }
    return true;
}

static bool handle_favourite_enable(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    int index = reg_id - REG_ID_FAVOURITE_ENABLE_0;
    if (index < 0 || index >= MAX_FAVOURITES) return false;

    bool enabled = (payload[2] != 0x00);

    ESP_LOGI(TAG, "%s Favourite %d (0x%02X) - %s", addr_info, index, reg_id,
             enabled ? "enabled" : "disabled");

    if (payload[2] > 0x01) {  // documented: 0x00 disabled, 0x01 enabled
        ESP_LOGW(TAG, "%s Favourite %d - undocumented enable value: 0x%02X", addr_info, index, payload[2]);
        record_undocumented(data, len);
    }

    pool_state_t state_snapshot;
    if (!state_lock(ctx, "favourite enable")) return true;
    ctx->pool_state->favourites[index].enabled = enabled;
    ctx->pool_state->favourites[index].enabled_valid = true;
    state_unlock(ctx, true, &state_snapshot);

    mqtt_publish_favourite(&state_snapshot);
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
 * Handler: Valve Sync — active valves bitmask (CMD 0x25)
 * The valve counterpart of handle_channels (CMD 0x0D): bit 0 = valve 1,
 * bit 1 = valve 2. Log-only; valve state comes from CMD 0x27.
 */
static bool handle_valve_sync(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t bitmask = payload[0];
    ESP_LOGI(TAG, "%s Active valves - 0x%02X [%c%c]",
             addr_info, bitmask,
             (bitmask & 0x02) ? '2' : '-',
             (bitmask & 0x01) ? '1' : '-');

    // Only bits 0-1 (two valve slots) are documented
    if (bitmask & ~0x03) {
        record_undocumented(data, len);
    }

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

    bool undocumented = false;  // set if we find any undocumented type/state

    uint8_t num_channels = payload[0];
    if (num_channels > MAX_CHANNELS) {
        ESP_LOGW(TAG, "%s Channel count %d exceeds MAX_CHANNELS (%d), clamping",
                 addr_info, num_channels, MAX_CHANNELS);
        undocumented = true;
        num_channels = MAX_CHANNELS;
    }
    ESP_LOGI(TAG, "%s Channel status (%d channels):", addr_info, num_channels);

    int payload_idx = 1;  // Channel data starts at payload[1]
    int ch_num = 1;
    bool past_end = false;
    uint8_t channels_to_publish[MAX_CHANNELS] = {0};
    int num_to_publish = 0;
    bool publish_pump = false;
    int filter_state = -1;  // Filter channel's reported state, -1 if it has none

    // Update pool state
    pool_state_t state_snapshot;
    if (state_lock(ctx, "channel status")) {
        ctx->pool_state->num_channels = num_channels;

        while (ch_num <= num_channels) {
            if (past_end || payload_idx + 2 >= payload_len) {
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                ch_num++;
                continue;
            }

            channel_type_t ch_type = (channel_type_t)payload[payload_idx];
            uint8_t state   = payload[payload_idx + 1];
            uint8_t active  = payload[payload_idx + 2];

            const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";

            if (ch_type == CHANNEL_TYPE_UNUSED) {
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                ctx->pool_state->channels[ch_num - 1].configured = false;
            } else {
                const char *type_name = get_channel_type_name(ch_type);
                ESP_LOGI(TAG, "  Ch%d: %s (%d) = %s (%s)", ch_num, type_name, ch_type, state_name,
                         active ? "Active" : "Inactive");

                // Flag undocumented channel type/state (PROTOCOL.md 0x0B). Only
                // checked for used channels — an unused channel's state byte
                // carries no documented meaning, mirroring handle_valve_state.
                if (state >= CHANNEL_STATE_COUNT || !channel_type_is_known(ch_type)) {
                    undocumented = true;
                }

                // Update channel state
                ctx->pool_state->channels[ch_num - 1].id = ch_num;
                ctx->pool_state->channels[ch_num - 1].type = ch_type;
                ctx->pool_state->channels[ch_num - 1].state = state;
                ctx->pool_state->channels[ch_num - 1].active = (active != 0);
                ctx->pool_state->channels[ch_num - 1].configured = true;
                // The dedicated Solar controller does not put its actual mode
                // or output state in CMD 0x0B (the tuple stays FC 00 00), so
                // the CMD 0x2C values replace it. Done after the undocumented
                // check above, which must see the raw state byte.
                apply_solar_status(ctx->pool_state, &ctx->pool_state->channels[ch_num - 1]);


                if (ch_type == CHANNEL_TYPE_FILTER) {
                    filter_state = state;
                }

                // If the Filter channel (CHANNEL_TYPE_FILTER) is no longer active
                // (e.g. turned Off manually, or turned off by a timer in Auto mode),
                // the pump loses power and won't broadcast a speed of 0. We must set it here.
                // Only for an install that actually has a telemetry-reporting pump:
                // pump_telemetry_seen is what says a CMD 0x3B broadcast has ever been
                // decoded. Without that check, a plain single-speed pump — which never
                // reports anything — would have speed/power marked valid the first time
                // the Filter channel went inactive, publishing HA pump entities that
                // can only ever read 0.
                if (ch_type == CHANNEL_TYPE_FILTER && !active && ctx->pool_state->pump_telemetry_seen) {
                    if (ctx->pool_state->pump_speed != 0 || !ctx->pool_state->pump_speed_valid ||
                        ctx->pool_state->pump_power_watts != 0 || !ctx->pool_state->pump_power_watts_valid) {
                        ctx->pool_state->pump_speed = 0;
                        ctx->pool_state->pump_speed_valid = true;
                        ctx->pool_state->pump_power_watts = 0;
                        ctx->pool_state->pump_power_watts_valid = true;
                        publish_pump = true;
                    }
                }

                // Mark this channel for publishing
                channels_to_publish[num_to_publish++] = ch_num;
            }

            payload_idx += 3;
            ch_num++;
        }

        state_unlock(ctx, true, &state_snapshot);
    }

    // Learn whether the Filter channel drives a multi-speed pump. Done outside
    // the state mutex because a change writes NVS, and before the publish below
    // so the channel's MQTT options are already correct when it goes out.
    if (filter_state >= 0) {
        filter_pump_type_learn((uint8_t)filter_state);
    }

    // Publish all channels using snapshot (outside mutex)
    if (ctx->enable_mqtt) {
        for (int i = 0; i < num_to_publish; i++) {
            mqtt_publish_channel(&state_snapshot, channels_to_publish[i]);
        }
        if (publish_pump) {
            mqtt_publish_pump(&state_snapshot);
        }
    }

    if (undocumented) {  // recorded outside the state_mutex critical section
        record_undocumented(data, len);
    }

    return true;
}

// ======================================================
// Viron XT Pump Telemetry (device 0x00A0) Handlers
// ======================================================

/**
 * Handler: Pump speed telemetry (CMD 0x3B, source 0x00A0).
 * Two-byte big-endian RPM value broadcast every ~60 seconds by the
 * Viron XT Variable Speed Pump. See PROTOCOL.md command `0x3B`.
 */
static bool handle_pump_speed(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len != 2 && payload_len != 4) return false;

    uint16_t speed_rpm = ((uint16_t)payload[0] << 8) | payload[1];
    
    uint16_t power_watts = 0;
    bool has_power = false;
    
    if (payload_len == 4) {
        power_watts = ((uint16_t)payload[2] << 8) | payload[3];
        has_power = true;
        ESP_LOGI(TAG, "%s Pump speed - %u RPM, Power - %u W", addr_info, speed_rpm, power_watts);
    } else {
        ESP_LOGI(TAG, "%s Pump speed - %u RPM", addr_info, speed_rpm);
    }

    pool_state_t snapshot;
    if (!state_lock(ctx, "pump speed")) return true;
    ctx->pool_state->pump_speed = speed_rpm;
    ctx->pool_state->pump_speed_valid = true;
    ctx->pool_state->pump_telemetry_seen = true;

    if (has_power) {
        ctx->pool_state->pump_power_watts = power_watts;
        ctx->pool_state->pump_power_watts_valid = true;
    }

    state_unlock(ctx, true, &snapshot);

    if (ctx->enable_mqtt) {
        mqtt_publish_pump(&snapshot);
    }

    return true;
}

/**
 * Handler: Pump speed command (Controller -> Pump)
 * Pattern: "02 00 50 00 A0 80 00 18 0D 97" or "02 00 84 00 A0 80 00 18 0D CB"
 *
 * Master speed preset command sent from the controller to the pump.
 * Mapping: 0x00=LOW, 0x01=MED, 0x02=HIGH.
 */
static bool handle_pump_speed_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t speed_preset_val = payload[0];
    const char *speed_preset_name = (speed_preset_val == 0x00) ? "Low" :
                                    (speed_preset_val == 0x01) ? "Med" :
                                    (speed_preset_val == 0x02) ? "High" : "Unknown";

    ESP_LOGI(TAG, "%s Set Pump speed - %s (0x%02X)",
             addr_info, speed_preset_name, speed_preset_val);

    // Only Low/Med/High (0x00-0x02) are documented; flag anything else.
    if (speed_preset_val > 0x02) {
        record_undocumented(data, len);
    }

    return true;
}

/**
 * Handler: Pump speed preset button activity notification (CMD 0x1B, source 0x00A0).
 * Emitted by the pump when physical speed preset buttons on pump are pressed.
 * Log-only — no pool_state update. See PROTOCOL.md command `0x1B`.
 */
static bool handle_pump_buttons(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    uint8_t activity = (payload_len >= 1) ? payload[0] : 0xFF;
    const char *button_name = (activity == 0x00) ? "Low" :
                              (activity == 0x01) ? "Med" :
                              (activity == 0x02) ? "High" : "Unknown";

    ESP_LOGI(TAG, "%s Speed button on Pump pressed - %s (0x%02X)",
             addr_info, button_name, activity);

    // Only Low/Med/High (0x00-0x02) are documented; flag any other button code.
    if (payload_len >= 1 && activity > 0x02) {
        record_undocumented(data, len);
    }
    return true;
}

/**
 * Handler: Connect 8/10 Controller -> Touchscreen heartbeat (CMD 0x2B)
 * Pattern: "02 00 62 00 50 80 00 2B 0E 6D"
 *
 * Unicast from the controller (0x0062) directly to the Touchscreen (0x0050)
 * roughly every 60 s. Purpose unknown; the 2-byte payload is 02 00 in every
 * observed capture. Log-only, but flag any deviation: this is one of only two
 * message types the controller sends straight to the touchscreen, making it a
 * prime candidate carrier for a controller-originated state signal (e.g.
 * service mode). See PROTOCOL.md command 0x2B.
 */
static bool handle_controller_heartbeat(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t b1 = payload[0];
    uint8_t b2 = payload[1];

    if (b1 != 0x02 || b2 != 0x00) {
        ESP_LOGW(TAG, "%s Controller heartbeat (CMD 0x2B) - UNEXPECTED VALUE: 0x%02X 0x%02X (expected 0x02 0x00)",
                 addr_info, b1, b2);
        record_undocumented(data, len);
    } else {
        ESP_LOGI(TAG, "%s Controller heartbeat (CMD 0x2B): 0x%02X 0x%02X", addr_info, b1, b2);
    }

    return true;
}

// ======================================================
// Main decoder function
// ======================================================

static bool dispatch_message(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);

// This function assumes the framing layer has validated that the message buffer
// has valid checksums, start and stop bytes etc. It does not repeat the validation.
// Note also that this function can be called directly by the test harness and from 
// the TX-echo path - in these cases it is incumbent on the caller to ensure that
// messages are valid.
bool decode_message(const uint8_t *data, int len, message_decoder_context_t *ctx)
{
    if (!ctx || !ctx->pool_state) {
        return false;
    }

    // Need enough bytes to read the frame type bytes (5-6) before classifying.
    if (len < 7) {
        unknown_buffer_record(data, len, UNKNOWN_REASON_BAD_FRAMING);
        return false;
    }

    // Classify by frame type bytes (5-6) to get the right minimum length: a
    // frame with no data is exactly the 10-byte header + end byte (11), with no
    // data-checksum byte; a frame with data always carries that extra byte too,
    // so its minimum is 12. See framing.c for the same classification at the
    // reassembly layer.
    framing_packet_type_t packet_type = framing_classify_packet(data[5], data[6]);
    int min_len = (packet_type == FRAME_NO_DATA) ? 11 : 12;
    if (len < min_len || data[0] != 0x02 || data[len - 1] != 0x03) {
        unknown_buffer_record(data, len, UNKNOWN_REASON_BAD_FRAMING);
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

    // Extract data payload section (bytes 10 to len-3)
    // Message format: [START=0][SRC=1-2][DST=3-4][FRAME_TYPE=5-6][CMD=7][LEN=8][HDR_CHK=9][DATA=10...][DATA_CHK=len-2][END=len-1]
    // A frame with no data has no payload and no data-checksum byte.
    const uint8_t *payload = &data[10];
    int payload_len = (packet_type == FRAME_NO_DATA) ? 0 : len - 12;  // len - (10 header bytes + data checksum + end)

    // Extract source and destination addresses
    uint8_t src_hi = data[1], src_lo = data[2];
    uint8_t dst_hi = data[3], dst_lo = data[4];
    char src_name_buf[16], dst_name_buf[16];
    const char *src_name = get_device_name(src_hi, src_lo, src_name_buf, sizeof(src_name_buf));
    const char *dst_name = get_device_name(dst_hi, dst_lo, dst_name_buf, sizeof(dst_name_buf));

    char addr_info[64];
    snprintf(addr_info, sizeof(addr_info), "[%s -> %s]", src_name, dst_name);

    // Register source address in the seen-devices registry (skip broadcast)
    if (!(src_hi == 0xFF && src_lo == 0xFF) && state_lock(ctx, "seen device registry")) {
        find_or_insert_seen_device_locked(ctx->pool_state, src_hi, src_lo);
        state_unlock(ctx, false, NULL);
    }

    bool decoded = dispatch_message(data, len, payload, payload_len, addr_info, ctx);

    // Increment global and per-device counters.
    bool record_frame = false;
    if (state_lock(ctx, "message counters")) {
        if (decoded) ctx->pool_state->messages_decoded_total++;
        else {
            ctx->pool_state->messages_unknown_total++;
            record_frame = true;
        }

        // Per-device counters (skip broadcast)
        if (!(src_hi == 0xFF && src_lo == 0xFF)) {
            int idx = find_or_insert_seen_device_locked(ctx->pool_state, src_hi, src_lo);
            if (idx >= 0) {
                if (decoded) ctx->pool_state->seen_devices[idx].decoded_count++;
                else         ctx->pool_state->seen_devices[idx].unknown_count++;
            }
        }
        state_unlock(ctx, false, NULL);
    }
    if (record_frame) unknown_buffer_record(data, len, UNKNOWN_REASON_UNHANDLED);

    return decoded;
}

/**
 * Router: Device status (CMD 0x12)
 *
 * Every device sends its own CMD 0x12 alongside its CMD 0x0A firmware
 * version, but the payload layout differs per source, so the handler is
 * chosen by source address. Unknown sources fall through to handle_unknown.
 */
static bool handle_device_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    uint16_t src = ((uint16_t)data[1] << 8) | data[2];

    switch (src) {
        case 0x0050:  // Touchscreen
            return handle_touchscreen_status(data, len, payload, payload_len, addr_info, ctx);
        case 0x0062:  // Connect 8/10 Controller
            return handle_heater(data, len, payload, payload_len, addr_info, ctx);
        case 0x0070:  // Genus Heater
            return handle_genus_heater_status(data, len, payload, payload_len, addr_info, ctx);
        case 0x0071:  // Viron Gas Heater
        case 0x0072:  // HiNRG Gas Heater
        case 0x0074:  // ICI Gas Heater
            return handle_gas_heater_status(data, len, payload, payload_len, addr_info, ctx);
        case 0x0081:  // VX 11S v3 Salt Chlorinator
            return handle_vx11s_status(data, len, payload, payload_len, addr_info, ctx);
        case 0x0084:  // Viron Chlorinator
        case 0x0090:  // RolaChem
            return handle_chlor_status(data, len, payload, payload_len, addr_info, ctx);
        case 0x00F0:  // Internet Gateway
            return handle_gateway_status(data, len, payload, payload_len, addr_info, ctx);
        default:
            return handle_unknown(data, len, payload, payload_len, addr_info, ctx);
    }
}

static bool dispatch_message(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    uint8_t cmd = data[7];

    // Firmware version (CMD 0x0A) — universal across sources; payload layout
    // is identical regardless of which device is broadcasting, so dispatched
    // by command byte rather than per-source pattern. See PROTOCOL.md
    // command `0x0A` section and the Known Command Bytes "Firmware version" row.
    if (cmd == 0x0A) {
        return handle_firmware_version(data, len, payload, payload_len, addr_info, ctx);
    }

    // Device status (CMD 0x12) — sent by each device alongside its 0x0A;
    // payload layout differs per source, so routed by source address. See
    // PROTOCOL.md command `0x12`.
    if (cmd == 0x12) {
        return handle_device_status(data, len, payload, payload_len, addr_info, ctx);
    }

    // Channel toggle command (CMD 0x10) — source-agnostic. Sent by the
    // Internet Gateway (0x00F0) for remote toggles and broadcast by the
    // Connect 8/10 controller (0x0062) when a channel button is pressed on
    // the controller itself; same 1-byte channel-index payload from either
    // source.
    if (cmd == 0x10) {
        return handle_channel_toggle_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    // Set channel state (CMD 0x0F) — source-agnostic. The Touchscreen does
    // not validate the source address at all; 0x0084 (Viron) and 0x0081
    // (VX 11S v3) are confirmed chlorinator sources on the bus, the Connect
    // 8/10 (0x0062) sends it for its Cleaning button, and this firmware
    // originates it as its own 0xAC1D.
    if (cmd == 0x0F) {
        return handle_set_channel_state(data, len, payload, payload_len, addr_info, ctx);
    }

    // Mode set command (CMD 0x15) — source-agnostic. 1-byte payload from the
    // Touchscreen (0x0050) sets the mode; the frame with no data from
    // the Connect 8/10 (0x0062) and the ICI Gas Heater (0x0074) toggles it.
    if (cmd == 0x15) {
        return handle_mode_set_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    // Valve button (CMD 0x29) — source-agnostic; only the Connect 8/10
    // (0x0062) observed sending it so far
    if (cmd == 0x29) {
        return handle_valve_button(data, len, payload, payload_len, addr_info, ctx);
    }

    // Internet Gateway info request (CMD 0x36) — source-agnostic; only the
    // Touchscreen (0x0050) observed sending it, to the Gateway (0x00F0)
    if (cmd == 0x36) {
        return handle_gateway_info_request(data, len, payload, payload_len, addr_info, ctx);
    }

    // Internet Gateway info (CMD 0x37) — source-agnostic; only the Gateway
    // (0x00F0) observed sending it. Routed by item ID, not LENGTH.
    if (cmd == 0x37) {
        return handle_gateway_info(data, len, payload, payload_len, addr_info, ctx);
    }

    // Valve state broadcast (CMD 0x27) — source-agnostic; only the
    // Touchscreen (0x0050) observed sending it so far. One layout whose
    // length follows the slot count in payload[0].
    if (cmd == 0x27) {
        return handle_valve_state(data, len, payload, payload_len, addr_info, ctx);
    }

    // Valve Sync (CMD 0x25) — source-agnostic; only the Touchscreen (0x0050)
    // observed sending it, to Internal Channels (0x006F)
    if (cmd == 0x25) {
        return handle_valve_sync(data, len, payload, payload_len, addr_info, ctx);
    }

    // Water temperature reading — CMD 0x16 (canonical) and CMD 0x31 (alt/
    // log-only) share the same {temp1, temp2} layout and are routed through
    // the same handler. See PROTOCOL.md commands `0x16` and `0x31`.
    if (cmd == 0x16 || cmd == 0x31) {
        return handle_temp_reading(data, len, payload, payload_len, addr_info, ctx);
    }

    // Pump speed command (CMD 0x18) — master speed preset command from
    // controller (Touchscreen 0x0050 or Viron Chlorinator 0x0084).
    if (cmd == 0x18) {
        return handle_pump_speed_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    // Temperature setpoint command (CMD 0x19) — source-agnostic. Routed by
    // the slot byte (payload[0]): 0x01/0x02 = Pool/Spa setpoint (3-byte
    // payload, Gateway-sourced); 0x03 = heater pair setpoint (5-byte payload
    // in °C + °F, Touchscreen-sourced to 0x007F).
    if (cmd == 0x19 && payload_len >= 1) {
        if (payload[0] == 0x01 || payload[0] == 0x02) {
            return handle_temp_set_cmd_pool_spa(data, len, payload, payload_len, addr_info, ctx);
        }
        if (payload[0] == 0x03) {
            return handle_temp_set_cmd_heaters(data, len, payload, payload_len, addr_info, ctx);
        }
    }

    // Chlorinator setpoint (CMD 0x1D) and reading (CMD 0x1F) — same
    // `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
    // 0x0084 Viron, only the source address (and resulting checksum1 byte)
    // differs. Dispatched by CMD byte and routed by the channel byte
    // (payload[0]): 0x01=pH, 0x02=ORP.
    if (cmd == 0x1D && payload_len >= 1) {
        if (payload[0] == 0x00) {
            return handle_chlor_output_level(data, len, payload, payload_len, addr_info, ctx);
        }
        if (payload[0] == 0x01) {
            return handle_chlor_ph_setpoint(data, len, payload, payload_len, addr_info, ctx);
        }
        if (payload[0] == 0x02) {
            return handle_chlor_orp_setpoint(data, len, payload, payload_len, addr_info, ctx);
        }
    }
    if (cmd == 0x1F && payload_len >= 1) {
        if (payload[0] == 0x01) {
            return handle_chlor_ph_reading(data, len, payload, payload_len, addr_info, ctx);
        }
        if (payload[0] == 0x02) {
            return handle_chlor_orp_reading(data, len, payload, payload_len, addr_info, ctx);
        }
    }

    // Favourite control command (CMD 0x2A) — source-agnostic. Sent to the
    // Touchscreen by the Gateway (0x00F0) for remote activations and by the
    // Connect 8/10 controller (0x0062) for activations made at the controller
    // itself; same 1-byte favourite-value payload from either source.
    if (cmd == 0x2A) {
        return handle_favourite_control_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    // Register read request (CMD 0x39) — source-agnostic. Observed from the
    // Internet Gateway (0x00F0) and the Genus Heater (0x0070); same
    // `{reg_id, slot_id}` payload from either source.
    if (cmd == 0x39) {
        return handle_register_read_request(data, len, payload, payload_len, addr_info, ctx);
    }

    // Register data (CMD 0x38) — source-agnostic. Routed by (reg_id, slot)
    // via REGISTER_HANDLERS. The Touchscreen (0x0050) is the canonical
    // responder to gateway 0x39 reads, but the same payload shape is used
    // wherever 0x38 originates.
    if (cmd == 0x38) {
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

    // Register write command (CMD 0x3A) — source-agnostic. Sent by the
    // Internet Gateway (0x00F0) for remote control and by the Viron
    // Chlorinator (0x0084), whose own app issues the same writes; same
    // {reg_id, slot, value} payload from either source.
    if (cmd == 0x3A) {
        return handle_register_write_request(data, len, payload, payload_len, addr_info, ctx);
    }

    // Lighting zone color broadcast (CMD 0x07) — dispatched on the CMD byte
    // alone; only the Touchscreen (0x0050) observed sending it so far
    if (cmd == 0x07) {
        return handle_light_color(data, len, payload, payload_len, addr_info, ctx);
    }

    // Light resync command (CMD 0x3C) — dispatched on the CMD byte alone;
    // only the Touchscreen (0x0050) observed sending it so far
    if (cmd == 0x3C) {
        return handle_light_resync(data, len, payload, payload_len, addr_info, ctx);
    }

    // Solar setpoint broadcast (CMD 0x2D) — dispatched on the CMD byte alone;
    // only the Touchscreen (0x0050) observed sending it so far
    if (cmd == 0x2D) {
        return handle_solar_setpoint_broadcast(data, len, payload, payload_len, addr_info, ctx);
    }

    // Solar status broadcast (CMD 0x2C) — dispatched on the CMD byte alone;
    // only the Touchscreen (0x0050) observed sending it so far
    if (cmd == 0x2C) {
        return handle_solar_status_broadcast(data, len, payload, payload_len, addr_info, ctx);
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

    if (match_pattern(data, len, MSG_TYPE_CONTROLLER_UNKNOWN2B)) {
        return handle_controller_heartbeat(data, len, payload, payload_len, addr_info, ctx);
    }

    // Heater temperature setting messages
    if (match_pattern(data, len, MSG_TYPE_GENUS_HEATER_TEMP_SETTING) ||
        match_pattern(data, len, MSG_TYPE_VIRON_HEATER_TEMP_SETTING) ||
        match_pattern(data, len, MSG_TYPE_HINRG_HEATER_TEMP_SETTING) || 
        match_pattern(data, len, MSG_TYPE_ICI_HEATER_TEMP_SETTING)) {
        return handle_heater_temp_setting(data, len, payload, payload_len, addr_info, ctx);
    }

    // Controller info messages
    if (match_pattern(data, len, MSG_TYPE_CONTROLLER_TIME)) {
        return handle_controller_time(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_PRE_VALVE_FRAME)) {
        return handle_pre_valve_frame(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_VALVE_ACTUATOR_CMD)) {
        return handle_valve_actuator_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN3)) {
        return handle_touchscreen_unknown3(data, len, payload, payload_len, addr_info, ctx);
    }

    // Viron Pump Telemetry (0x00A0) messages
    if (match_pattern(data, len, MSG_TYPE_PUMP_SPEED) ||
        match_pattern(data, len, MSG_TYPE_PUMP_SPEED_V2)) {
        return handle_pump_speed(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_PUMP_BUTTONS)) {
        return handle_pump_buttons(data, len, payload, payload_len, addr_info, ctx);
    }

    // No handler matched - log as unknown
    return handle_unknown(data, len, payload, payload_len, addr_info, ctx);
}
