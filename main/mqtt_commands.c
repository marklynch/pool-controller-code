#include "mqtt_commands.h"
#include "mqtt_poolclient.h"
#include "esp_log.h"
#include "driver/uart.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "MQTT_COMMANDS";

// UART configuration (should match main.c)
#define BUS_UART_NUM    UART_NUM_1

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

    // Determine ON/OFF
    bool turn_on = (strncmp(payload, "ON", payload_len) == 0);

    // TODO: Build UART message to toggle channel
    // This requires reverse-engineering the pool controller's command protocol
    // For now, just log the command
    ESP_LOGW(TAG, "Channel control not yet implemented - need UART command bytes");
    ESP_LOGI(TAG, "Would %s channel %d", turn_on ? "turn ON" : "turn OFF", channel_id);

    // Example placeholder (replace with actual command bytes):
    // uint8_t cmd[] = {0x02, 0x00, 0x6F, 0x00, 0x50, 0x80, 0x00, 0x0C, 0x0D, channel_byte, state_byte, checksum, 0x03};
    // send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Light Control
// ======================================================

static void handle_light_command(int zone, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Light zone %d command: %.*s", zone, payload_len, payload);

    // Determine ON/OFF
    bool turn_on = (strncmp(payload, "ON", payload_len) == 0);

    // TODO: Build UART message to toggle light
    ESP_LOGW(TAG, "Light control not yet implemented - need UART command bytes");
    ESP_LOGI(TAG, "Would %s light zone %d", turn_on ? "turn ON" : "turn OFF", zone);

    // Example placeholder:
    // uint8_t cmd[] = {...};
    // send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Mode Control
// ======================================================

static void handle_mode_command(const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Mode command: %.*s", payload_len, payload);

    // Determine Pool/Spa
    bool set_pool = (strncmp(payload, "Pool", payload_len) == 0);

    // TODO: Build UART message to switch mode
    ESP_LOGW(TAG, "Mode control not yet implemented - need UART command bytes");
    ESP_LOGI(TAG, "Would switch to %s mode", set_pool ? "Pool" : "Spa");

    // Example placeholder:
    // uint8_t cmd[] = {...};
    // send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Temperature Setpoint Control
// ======================================================

static void handle_temperature_command(bool is_pool, const char *payload, int payload_len)
{
    // Parse temperature value
    char temp_str[16];
    if (payload_len >= sizeof(temp_str)) {
        ESP_LOGE(TAG, "Temperature payload too long");
        return;
    }
    memcpy(temp_str, payload, payload_len);
    temp_str[payload_len] = '\0';

    int temp = atoi(temp_str);
    ESP_LOGI(TAG, "%s setpoint command: %d", is_pool ? "Pool" : "Spa", temp);

    // Validate temperature range
    if (temp < 50 || temp > 104) {
        ESP_LOGE(TAG, "Temperature out of range: %d", temp);
        return;
    }

    // TODO: Build UART message to set temperature
    ESP_LOGW(TAG, "Temperature control not yet implemented - need UART command bytes");
    ESP_LOGI(TAG, "Would set %s setpoint to %d°F", is_pool ? "pool" : "spa", temp);

    // Example placeholder:
    // uint8_t cmd[] = {..., (uint8_t)temp, ...};
    // send_uart_command(cmd, sizeof(cmd));
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
        int channel = cmd_topic[8] - '0';
        if (channel >= 1 && channel <= 8) {
            handle_channel_command(channel, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid channel number: %d", channel);
        }
    }
    else if (strncmp(cmd_topic, "light/", 6) == 0 && cmd_topic_len > 10) {
        // Extract zone number (format: "light/N/set")
        int zone = cmd_topic[6] - '0';
        if (zone >= 1 && zone <= 4) {
            handle_light_command(zone, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid light zone: %d", zone);
        }
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
