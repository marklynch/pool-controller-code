#include "mqtt_commands.h"
#include "config.h"
#include "mqtt_poolclient.h"
#include "pool_state.h"
#include "esp_log.h"
#include "driver/uart.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

static const char *TAG = "MQTT_COMMANDS";

// ======================================================
// UART Command Helpers
// ======================================================

// Send raw UART message to pool bus
static void send_uart_command(const uint8_t *data, size_t len)
{
    int written = uart_write_bytes(BUS_UART_NUM, (const char *)data, len);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write to UART");
    } else {
        ESP_LOGI(TAG, "Sent UART command (%d bytes)", written);
    }
}

// ======================================================
// Channel Control
// ======================================================

static void handle_channel_command(int channel_id, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Channel %d command: %.*s", channel_id, payload_len, payload);

    uint8_t channel_idx = channel_id - 1;

    if (channel_idx >= MAX_CHANNELS) {
        ESP_LOGE(TAG, "Channel %d out of range", channel_id);
        return;
    }

    // Build toggle command
    // Pattern: 02 00 F0 FF FF 80 00 10 0D 8D [CHANNEL_IDX] [CHECKSUM] 03
    // Checksum = channel_idx (only data byte)
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x10, 0x0D, 0x8D, // Command pattern
        channel_idx,       // Channel index (0-based)
        channel_idx,       // Checksum (= channel index)
        0x03               // END
    };

    ESP_LOGI(TAG, "Toggling channel %d", channel_id);
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Light Control
// ======================================================

static void handle_light_command(int zone, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Light zone %d command: %.*s", zone, payload_len, payload);

    // Determine state: OFF=0x00, AUTO=0x01, ON=0x02
    uint8_t state;
    if (strncmp(payload, "ON", payload_len) == 0) {
        state = 0x02;
    } else if (strncmp(payload, "OFF", payload_len) == 0) {
        state = 0x00;
    } else if (strncmp(payload, "AUTO", payload_len) == 0) {
        state = 0x01;
    } else {
        ESP_LOGE(TAG, "Invalid light command: %.*s (expected ON/OFF/AUTO)", payload_len, payload);
        return;
    }

    // Calculate register ID (0xC0 for zone 1, 0xC1 for zone 2, etc.)
    uint8_t reg_id = 0xC0 + (zone - 1);

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG_ID] 01 [STATE] [CHECKSUM] 03
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x3A, 0x0F, 0xB9, // Command pattern
        reg_id,     // Register ID (light zone)
        0x01,       // Slot ID (state)
        state,      // State value (OFF/AUTO/ON)
        0x00,       // Checksum (calculated below)
        0x03        // END
    };

    // Calculate checksum (sum of bytes 10-12)
    cmd[13] = (reg_id + 0x01 + state) & 0xFF;

    ESP_LOGI(TAG, "Sending light zone %d %s command", zone,
             state == 0x02 ? "ON" : (state == 0x00 ? "OFF" : "AUTO"));
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Heater Control
// ======================================================

static void handle_heater_command(const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Heater command: %.*s", payload_len, payload);

    uint8_t state;
    if (strncmp(payload, "ON", payload_len) == 0) {
        state = 0x01;
    } else if (strncmp(payload, "OFF", payload_len) == 0) {
        state = 0x00;
    } else {
        ESP_LOGE(TAG, "Invalid heater command: %.*s (expected ON/OFF)", payload_len, payload);
        return;
    }

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 E6 00 [STATE] [CHECKSUM] 03
    // Checksum = 0xE6 + 0x00 + state
    uint8_t cmd[] = {
        0x02,             // START
        0x00, 0xF0,       // SOURCE: Internet Gateway
        0xFF, 0xFF,       // DEST: Broadcast
        0x80, 0x00,       // CONTROL
        0x3A, 0x0F, 0xB9, // Command pattern
        0xE6,             // Register ID (heater)
        0x00,             // Slot
        state,            // State (0x00=Off, 0x01=On)
        (0xE6 + 0x00 + state) & 0xFF, // Checksum
        0x03              // END
    };

    ESP_LOGI(TAG, "Sending heater %s command", state ? "ON" : "OFF");
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Mode Control
// ======================================================

static void handle_mode_command(const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Mode command: %.*s", payload_len, payload);

    // Determine mode value
    // Note: Command values are inverted from status values
    // Status: Spa=0x00, Pool=0x01
    // Command: Spa=0x01, Pool=0x00
    uint8_t mode_value;
    if (strncmp(payload, "Pool", payload_len) == 0) {
        mode_value = 0x00;  // Switch to Pool mode
    } else if (strncmp(payload, "Spa", payload_len) == 0) {
        mode_value = 0x01;  // Switch to Spa mode
    } else {
        ESP_LOGE(TAG, "Invalid mode command: %.*s (expected Pool/Spa)", payload_len, payload);
        return;
    }

    // Build UART command
    // Pattern: 02 00 F0 00 50 80 00 2A 0D F9 [MODE] [CHECKSUM] 03
    // Note: Destination is Touch Screen (0x0050), not broadcast
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0x00, 0x50, // DEST: Touch Screen (not broadcast!)
        0x80, 0x00, // CONTROL
        0x2A, 0x0D, 0xF9, // Command pattern
        mode_value, // Mode value (Pool=0x00, Spa=0x01)
        mode_value, // Checksum (just the mode value)
        0x03        // END
    };

    ESP_LOGI(TAG, "Sending mode switch to %s", mode_value == 0x01 ? "Spa" : "Pool");
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Temperature Setpoint Control
// ======================================================

static void handle_temperature_command(bool is_pool, const char *payload, int payload_len)
{
    // Parse temperature value (Celsius)
    char temp_str[16];
    if (payload_len >= sizeof(temp_str)) {
        ESP_LOGE(TAG, "Temperature payload too long");
        return;
    }
    memcpy(temp_str, payload, payload_len);
    temp_str[payload_len] = '\0';

    char *endptr;
    long temp_c = strtol(temp_str, &endptr, 10);
    if (endptr == temp_str || *endptr != '\0') {
        ESP_LOGE(TAG, "Invalid temperature value: \"%s\"", temp_str);
        return;
    }
    ESP_LOGI(TAG, "%s setpoint command: %ld°C", is_pool ? "Pool" : "Spa", temp_c);

    // Validate temperature range (Celsius)
    if (temp_c < 15 || temp_c > 42) {
        ESP_LOGE(TAG, "Temperature out of range: %ld°C (valid: 15-42)", temp_c);
        return;
    }

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 19 0F 98 [TARGET] [TEMP_C] [TEMP_C] [CHECKSUM] 03
    // TARGET: 0x01=Pool, 0x02=Spa
    // Temperature byte is repeated as part of the message format
    // Checksum = TARGET + TEMP_C + TEMP_C
    uint8_t target = is_pool ? 0x01 : 0x02;
    uint8_t temp_byte = (uint8_t)temp_c;
    uint8_t checksum = (target + temp_byte + temp_byte) & 0xFF;

    uint8_t cmd[] = {
        0x02,             // START
        0x00, 0xF0,       // SOURCE: Internet Gateway
        0xFF, 0xFF,       // DEST: Broadcast
        0x80, 0x00,       // CONTROL
        0x19, 0x0F, 0x98, // Command pattern
        target,           // Target (0x01=Pool, 0x02=Spa)
        temp_byte,        // Temperature °C
        temp_byte,        // Temperature °C (repeated)
        checksum,         // Checksum
        0x03              // END
    };

    ESP_LOGI(TAG, "Setting %s setpoint to %" PRId32 "°C", is_pool ? "pool" : "spa", temp_c);
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Valve Control
// ======================================================

static void handle_valve_command(int valve_num, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Valve %d command: %.*s", valve_num, payload_len, payload);

    uint8_t state;
    if (strncmp(payload, "On", payload_len) == 0) {
        state = 0x02;
    } else if (strncmp(payload, "Auto", payload_len) == 0) {
        state = 0x01;
    } else if (strncmp(payload, "Off", payload_len) == 0) {
        state = 0x00;
    } else {
        ESP_LOGE(TAG, "Invalid valve command: %.*s (expected Off/Auto/On)", payload_len, payload);
        return;
    }

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 28 0E A6 [VALVE_IDX] [STATE] [CHECKSUM] 03
    // Checksum = (valve_idx + state) & 0xFF
    uint8_t valve_idx = valve_num - 1;
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x28, 0x0E, 0xA6, // Command pattern (checksum: 02+00+F0+FF+FF+80+00+28+0E = 0xA6)
        valve_idx,         // Valve index (0-based)
        state,             // Target state (0=Off, 1=Auto, 2=On)
        (valve_idx + state) & 0xFF, // Data checksum
        0x03               // END
    };

    ESP_LOGI(TAG, "Setting valve %d to %s", valve_num,
             state == 0x02 ? "On" : (state == 0x01 ? "Auto" : "Off"));
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Main Command Handler
// ======================================================

void mqtt_handle_command(const char *topic, int topic_len, const char *data, int data_len)
{
    // Get device ID for topic matching
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    // Build expected topic prefix
    char topic_prefix[64];
    snprintf(topic_prefix, sizeof(topic_prefix), "pool/%s/", device_id);
    int prefix_len = strlen(topic_prefix);

    // Check if topic matches our device
    if (topic_len < prefix_len || strncmp(topic, topic_prefix, prefix_len) != 0) {
        ESP_LOGW(TAG, "Topic does not match device ID");
        return;
    }

    // Extract command part (after device prefix)
    const char *cmd_topic = topic + prefix_len;
    int cmd_topic_len = topic_len - prefix_len;

    // Parse command type
    if (strncmp(cmd_topic, "channel/", 8) == 0 && cmd_topic_len > 12) {
        // Extract channel number (format: "channel/N/set")
        if (!isdigit((unsigned char)cmd_topic[8])) {
            ESP_LOGE(TAG, "Invalid channel topic format: %s", cmd_topic);
            return;
        }
        int channel = cmd_topic[8] - '0';
        if (channel >= 1 && channel <= MAX_CHANNELS) {
            handle_channel_command(channel, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid channel number: %d", channel);
        }
    }
    else if (strncmp(cmd_topic, "light/", 6) == 0 && cmd_topic_len > 10) {
        // Extract zone number (format: "light/N/set")
        if (!isdigit((unsigned char)cmd_topic[6])) {
            ESP_LOGE(TAG, "Invalid light topic format: %s", cmd_topic);
            return;
        }
        int zone = cmd_topic[6] - '0';
        if (zone >= 1 && zone <= MAX_LIGHT_ZONES) {
            handle_light_command(zone, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid light zone: %d", zone);
        }
    }
    else if (strncmp(cmd_topic, "valve/", 6) == 0 && cmd_topic_len > 8) {
        // Extract valve number (format: "valve/N/set")
        if (!isdigit((unsigned char)cmd_topic[6])) {
            ESP_LOGE(TAG, "Invalid valve topic format: %s", cmd_topic);
            return;
        }
        int valve = cmd_topic[6] - '0';
        if (valve >= 1 && valve <= MAX_VALVE_SLOTS) {
            handle_valve_command(valve, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid valve number: %d", valve);
        }
    }
    else if (strncmp(cmd_topic, "heater/set", 10) == 0) {
        handle_heater_command(data, data_len);
    }
    else if (strncmp(cmd_topic, "mode/set", 8) == 0) {
        handle_mode_command(data, data_len);
    }
    else if (strncmp(cmd_topic, "temperature/pool/set", 20) == 0) {
        handle_temperature_command(true, data, data_len);
    }
    else if (strncmp(cmd_topic, "temperature/spa/set", 19) == 0) {
        handle_temperature_command(false, data, data_len);
    }
    else {
        ESP_LOGW(TAG, "Unknown command topic: %.*s", cmd_topic_len, cmd_topic);
    }
}
