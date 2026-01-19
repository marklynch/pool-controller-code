#include "mqtt_publish.h"
#include "mqtt_poolclient.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "MQTT_PUBLISH";

// Last published state (for change detection)
static struct {
    uint8_t current_temp;
    uint8_t pool_setpoint;
    uint8_t spa_setpoint;
    bool temp_valid;

    bool heater_on;
    bool heater_valid;

    uint8_t mode;
    bool mode_valid;

    struct {
        uint8_t type;
        uint8_t state;
        char name[32];
        bool valid;
    } channels[8];

    struct {
        uint8_t state;
        uint8_t color;
        bool active;
        bool valid;
    } lights[4];

    uint16_t ph_reading;
    uint16_t orp_reading;
    bool ph_valid;
    bool orp_valid;
} s_last_published = {0};

// ======================================================
// Temperature Publishing
// ======================================================

void mqtt_publish_temperature(uint8_t current_temp, uint8_t pool_setpoint, uint8_t spa_setpoint, bool temp_scale_fahrenheit)
{
    // Check if anything changed
    if (s_last_published.temp_valid &&
        s_last_published.current_temp == current_temp &&
        s_last_published.pool_setpoint == pool_setpoint &&
        s_last_published.spa_setpoint == spa_setpoint) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/temperature/state", device_id);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"current\":%d,\"pool_sp\":%d,\"spa_sp\":%d,\"scale\":\"%s\"}",
             current_temp, pool_setpoint, spa_setpoint,
             temp_scale_fahrenheit ? "F" : "C");

    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published.current_temp = current_temp;
    s_last_published.pool_setpoint = pool_setpoint;
    s_last_published.spa_setpoint = spa_setpoint;
    s_last_published.temp_valid = true;

    ESP_LOGI(TAG, "Published temperature: %d°%s (pool_sp=%d, spa_sp=%d)",
             current_temp, temp_scale_fahrenheit ? "F" : "C", pool_setpoint, spa_setpoint);
}

// ======================================================
// Heater Publishing
// ======================================================

void mqtt_publish_heater(bool heater_on)
{
    // Check if anything changed
    if (s_last_published.heater_valid && s_last_published.heater_on == heater_on) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/heater/state", device_id);

    const char *payload = heater_on ? "ON" : "OFF";
    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published.heater_on = heater_on;
    s_last_published.heater_valid = true;

    ESP_LOGI(TAG, "Published heater: %s", payload);
}

// ======================================================
// Mode Publishing
// ======================================================

void mqtt_publish_mode(uint8_t mode)
{
    // Check if anything changed
    if (s_last_published.mode_valid && s_last_published.mode == mode) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/mode/state", device_id);

    const char *payload = (mode == 0) ? "Spa" : (mode == 1) ? "Pool" : "Unknown";
    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published.mode = mode;
    s_last_published.mode_valid = true;

    ESP_LOGI(TAG, "Published mode: %s", payload);
}

// ======================================================
// Channel Publishing
// ======================================================

void mqtt_publish_channel(uint8_t channel_id, uint8_t type, uint8_t state, const char *name)
{
    if (channel_id < 1 || channel_id > 8) {
        return;
    }

    int idx = channel_id - 1;

    // Check if anything changed
    if (s_last_published.channels[idx].valid &&
        s_last_published.channels[idx].type == type &&
        s_last_published.channels[idx].state == state &&
        strcmp(s_last_published.channels[idx].name, name) == 0) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/channel/%d/state", device_id, channel_id);

    // State names
    static const char *STATE_NAMES[] = {"Off", "Auto", "On", "Low", "Medium", "High"};
    const char *state_name = (state < 6) ? STATE_NAMES[state] : "Unknown";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"name\":\"%s\"}",
             state_name, name);

    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published.channels[idx].type = type;
    s_last_published.channels[idx].state = state;
    strncpy(s_last_published.channels[idx].name, name, sizeof(s_last_published.channels[idx].name) - 1);
    s_last_published.channels[idx].valid = true;

    ESP_LOGI(TAG, "Published channel %d: %s (%s)", channel_id, state_name, name);
}

// ======================================================
// Lighting Publishing
// ======================================================

void mqtt_publish_light(uint8_t zone, uint8_t state, uint8_t color, bool active)
{
    if (zone < 1 || zone > 4) {
        return;
    }

    int idx = zone - 1;

    // Check if anything changed
    if (s_last_published.lights[idx].valid &&
        s_last_published.lights[idx].state == state &&
        s_last_published.lights[idx].color == color &&
        s_last_published.lights[idx].active == active) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/light/%d/state", device_id, zone);

    // State names
    static const char *STATE_NAMES[] = {"Off", "Auto", "On"};
    const char *state_name = (state < 3) ? STATE_NAMES[state] : "Unknown";

    // Color names (subset - full list is in main.c)
    static const char *COLOR_NAMES[] = {
        "Unknown", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "White",
        "User1", "User2", "Disco", "Smooth", "Fade", "Magenta", "Cyan"
    };
    const char *color_name = (color < 15) ? COLOR_NAMES[color] : "Unknown";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"color\":\"%s\",\"active\":%s}",
             state_name, color_name, active ? "true" : "false");

    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published.lights[idx].state = state;
    s_last_published.lights[idx].color = color;
    s_last_published.lights[idx].active = active;
    s_last_published.lights[idx].valid = true;

    ESP_LOGI(TAG, "Published light %d: %s, %s, active=%d", zone, state_name, color_name, active);
}

// ======================================================
// Chlorinator Publishing
// ======================================================

void mqtt_publish_chlorinator(uint16_t ph_reading, uint16_t orp_reading, bool ph_valid, bool orp_valid)
{

    ESP_LOGI(TAG, "Prepare to Publish chlorinator: pH=%d (valid=%d), ORP=%d (valid=%d)",
             ph_reading, ph_valid, orp_reading, orp_valid  );

    // Check if anything changed
    if (s_last_published.ph_valid == ph_valid &&
        s_last_published.orp_valid == orp_valid &&
        s_last_published.ph_reading == ph_reading &&
        s_last_published.orp_reading == orp_reading) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/chlorinator/state", device_id);

    char payload[256];
    if (ph_valid && orp_valid) {
        snprintf(payload, sizeof(payload),
                 "{\"ph\":%.1f,\"orp\":%d}",
                 ph_reading / 10.0, orp_reading);
    } else if (ph_valid) {
        snprintf(payload, sizeof(payload),
                 "{\"ph\":%.1f,\"orp\":null}",
                 ph_reading / 10.0);
    } else if (orp_valid) {
        snprintf(payload, sizeof(payload),
                 "{\"ph\":null,\"orp\":%d}",
                 orp_reading);
    } else {
        snprintf(payload, sizeof(payload), "{\"ph\":null,\"orp\":null}");
    }

    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published.ph_reading = ph_reading;
    s_last_published.orp_reading = orp_reading;
    s_last_published.ph_valid = ph_valid;
    s_last_published.orp_valid = orp_valid;

    ESP_LOGI(TAG, "Published chlorinator: pH=%.1f, ORP=%d", ph_reading / 10.0, orp_reading);
}
