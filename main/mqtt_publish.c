#include "mqtt_publish.h"
#include "mqtt_poolclient.h"
#include "mqtt_discovery.h"
#include "pool_state.h"
#include "message_decoder.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "MQTT_PUBLISH";

// Last published state (for change detection) - using pool_state_t as single source of truth
static pool_state_t s_last_published_state = {0};

// Track whether discovery has been published for each channel/light
static struct {
    bool channels[8];
    bool lights[4];
} s_discovery_published = {0};

// ======================================================
// Temperature Publishing
// ======================================================

void mqtt_publish_temperature(const pool_state_t *current_state)
{
    // Check if anything changed
    if (s_last_published_state.temp_valid &&
        s_last_published_state.current_temp == current_state->current_temp &&
        s_last_published_state.pool_setpoint == current_state->pool_setpoint &&
        s_last_published_state.spa_setpoint == current_state->spa_setpoint) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/temperature/state", device_id);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"current\":%d,\"pool_sp\":%d,\"spa_sp\":%d,\"scale\":\"%s\"}",
             current_state->current_temp, current_state->pool_setpoint, current_state->spa_setpoint,
             current_state->temp_scale_fahrenheit ? "F" : "C");

    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published_state.current_temp = current_state->current_temp;
    s_last_published_state.pool_setpoint = current_state->pool_setpoint;
    s_last_published_state.spa_setpoint = current_state->spa_setpoint;
    s_last_published_state.temp_valid = true;

    ESP_LOGI(TAG, "Published temperature: %d°%s (pool_sp=%d, spa_sp=%d)",
             current_state->current_temp, current_state->temp_scale_fahrenheit ? "F" : "C",
             current_state->pool_setpoint, current_state->spa_setpoint);
}

// ======================================================
// Heater Publishing
// ======================================================

void mqtt_publish_heater(const pool_state_t *current_state)
{
    // Check if anything changed
    if (s_last_published_state.heater_valid && s_last_published_state.heater_on == current_state->heater_on) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/heater/state", device_id);

    const char *payload = current_state->heater_on ? "ON" : "OFF";
    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published_state.heater_on = current_state->heater_on;
    s_last_published_state.heater_valid = true;

    ESP_LOGI(TAG, "Published heater: %s", payload);
}

// ======================================================
// Mode Publishing
// ======================================================

void mqtt_publish_mode(const pool_state_t *current_state)
{
    // Check if anything changed
    if (s_last_published_state.mode_valid && s_last_published_state.mode == current_state->mode) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/mode/state", device_id);

    const char *payload = (current_state->mode == 0) ? "Spa" : (current_state->mode == 1) ? "Pool" : "Unknown";
    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published_state.mode = current_state->mode;
    s_last_published_state.mode_valid = true;

    ESP_LOGI(TAG, "Published mode: %s", payload);
}

// ======================================================
// Channel Publishing
// ======================================================

void mqtt_publish_channel(const pool_state_t *current_state, uint8_t channel_id)
{
    if (channel_id < 1 || channel_id > 8) {
        return;
    }

    int idx = channel_id - 1;
    const channel_state_t *channel = &current_state->channels[idx];

    // Skip unconfigured/unused channels
    if (!channel->configured) {
        ESP_LOGD(TAG, "Skipping unconfigured channel %d", channel_id);
        return;
    }

    // Use channel name if set, otherwise fall back to type name
    const char *type_name = get_channel_type_name(channel->type);
    const char *display_name = (channel->name[0] != '\0') ? channel->name : type_name;

    // Publish discovery if this is the first time seeing this channel
    if (!s_discovery_published.channels[idx]) {
        mqtt_publish_channel_discovery_single(channel_id, display_name);
        s_discovery_published.channels[idx] = true;
    }

    // Check if anything changed
    if (s_last_published_state.channels[idx].configured &&
        s_last_published_state.channels[idx].type == channel->type &&
        s_last_published_state.channels[idx].state == channel->state &&
        strcmp(s_last_published_state.channels[idx].name, channel->name) == 0) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/channel/%d/state", device_id, channel_id);

    // State names
    static const char *STATE_NAMES[] = {"Off", "Auto", "On", "Low", "Medium", "High"};
    const char *state_name = (channel->state < 6) ? STATE_NAMES[channel->state] : "Unknown";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"name\":\"%s\"}",
             state_name, display_name);

    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published_state.channels[idx].type = channel->type;
    s_last_published_state.channels[idx].state = channel->state;
    strncpy(s_last_published_state.channels[idx].name, channel->name, sizeof(s_last_published_state.channels[idx].name) - 1);
    s_last_published_state.channels[idx].configured = true;

    ESP_LOGI(TAG, "Published channel %d: %s (%s)", channel_id, state_name, display_name);
}

// ======================================================
// Lighting Publishing
// ======================================================

void mqtt_publish_light(const pool_state_t *current_state, uint8_t zone)
{
    if (zone < 1 || zone > 4) {
        return;
    }

    int idx = zone - 1;
    const lighting_state_t *light = &current_state->lighting[idx];

    // Skip unconfigured/unused lighting zones
    if (!light->configured) {
        ESP_LOGD(TAG, "Skipping unconfigured light zone %d", zone);
        return;
    }

    // Publish discovery if this is the first time seeing this light zone
    if (!s_discovery_published.lights[idx]) {
        mqtt_publish_light_discovery_single(zone);
        s_discovery_published.lights[idx] = true;
    }

    // Check if anything changed
    if (s_last_published_state.lighting[idx].configured &&
        s_last_published_state.lighting[idx].state == light->state &&
        s_last_published_state.lighting[idx].color == light->color &&
        s_last_published_state.lighting[idx].active == light->active) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/light/%d/state", device_id, zone);

    // State names
    static const char *STATE_NAMES[] = {"Off", "Auto", "On"};
    const char *state_name = (light->state < 3) ? STATE_NAMES[light->state] : "Unknown";

    // Color names (subset - full list is in main.c)
    static const char *COLOR_NAMES[] = {
        "Unknown", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "White",
        "User1", "User2", "Disco", "Smooth", "Fade", "Magenta", "Cyan"
    };
    const char *color_name = (light->color < 15) ? COLOR_NAMES[light->color] : "Unknown";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"color\":\"%s\",\"active\":%s}",
             state_name, color_name, light->active ? "true" : "false");

    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published_state.lighting[idx].state = light->state;
    s_last_published_state.lighting[idx].color = light->color;
    s_last_published_state.lighting[idx].active = light->active;
    s_last_published_state.lighting[idx].configured = true;

    ESP_LOGI(TAG, "Published light %d: %s, %s, active=%d", zone, state_name, color_name, light->active);
}

// ======================================================
// Chlorinator Publishing
// ======================================================

void mqtt_publish_chlorinator(const pool_state_t *current_state)
{
    ESP_LOGI(TAG, "Prepare to Publish chlorinator: pH=%d (valid=%d), ORP=%d (valid=%d), pH_sp=%d, ORP_sp=%d",
             current_state->ph_reading, current_state->ph_valid,
             current_state->orp_reading, current_state->orp_valid,
             current_state->ph_setpoint, current_state->orp_setpoint);

    // Check if anything changed
    if (s_last_published_state.ph_valid == current_state->ph_valid &&
        s_last_published_state.orp_valid == current_state->orp_valid &&
        s_last_published_state.ph_reading == current_state->ph_reading &&
        s_last_published_state.orp_reading == current_state->orp_reading &&
        s_last_published_state.ph_setpoint == current_state->ph_setpoint &&
        s_last_published_state.orp_setpoint == current_state->orp_setpoint) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/chlorinator/state", device_id);

    char payload[512];
    int len = snprintf(payload, sizeof(payload), "{");

    // pH reading
    if (current_state->ph_valid) {
        len += snprintf(payload + len, sizeof(payload) - len,
                       "\"ph\":%.1f", current_state->ph_reading / 10.0);
    } else {
        len += snprintf(payload + len, sizeof(payload) - len, "\"ph\":null");
    }

    // pH setpoint
    len += snprintf(payload + len, sizeof(payload) - len,
                   ",\"ph_setpoint\":%.1f", current_state->ph_setpoint / 10.0);

    // ORP reading
    if (current_state->orp_valid) {
        len += snprintf(payload + len, sizeof(payload) - len,
                       ",\"orp\":%d", current_state->orp_reading);
    } else {
        len += snprintf(payload + len, sizeof(payload) - len, ",\"orp\":null");
    }

    // ORP setpoint
    len += snprintf(payload + len, sizeof(payload) - len,
                   ",\"orp_setpoint\":%d}", current_state->orp_setpoint);

    mqtt_publish(topic, payload, 0, false);

    // Update last published state
    s_last_published_state.ph_reading = current_state->ph_reading;
    s_last_published_state.orp_reading = current_state->orp_reading;
    s_last_published_state.ph_setpoint = current_state->ph_setpoint;
    s_last_published_state.orp_setpoint = current_state->orp_setpoint;
    s_last_published_state.ph_valid = current_state->ph_valid;
    s_last_published_state.orp_valid = current_state->orp_valid;

    ESP_LOGI(TAG, "Published chlorinator: pH=%.1f (sp=%.1f), ORP=%d (sp=%d)",
             current_state->ph_reading / 10.0, current_state->ph_setpoint / 10.0,
             current_state->orp_reading, current_state->orp_setpoint);
}
