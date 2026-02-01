#include "mqtt_discovery.h"
#include "config.h"
#include "mqtt_poolclient.h"
#include "pool_state.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "MQTT_DISCOVERY";

// Helper function to build device JSON
static void build_device_json(char *device_json, size_t max_len, const char *device_id)
{
    snprintf(device_json, max_len,
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"Astral Pool Controller\","
             "\"model\":\"ESP32-C6 Bridge\",\"manufacturer\":\"Mark Lynch\"}",
             device_id);
}

// Helper function to publish discovery message
static void publish_discovery(const char *component, const char *object_id, const char *config_json)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[256];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", component, device_id, object_id);

    mqtt_publish(topic, config_json, 1, true);
    ESP_LOGI(TAG, "Published discovery: %s/%s", component, object_id);
}

// ======================================================
// Temperature Sensor Discovery
// ======================================================

static void publish_temperature_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/temperature/state", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"Pool Temperature\",\"device_class\":\"temperature\","
             "\"icon\": \"mdi:thermometer\","
             "\"state_topic\":\"%s\",\"unit_of_measurement\":\"°C\","
             "\"value_template\":\"{{ value_json.current }}\","
             "\"unique_id\":\"%s_temp\",\"availability_topic\":\"%s\",%s}",
             state_topic, device_id, avail_topic, device_json);

    publish_discovery("sensor", "temperature", config);
}

// ======================================================
// Pool Setpoint Number Discovery
// ======================================================

static void publish_pool_setpoint_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/temperature/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/temperature/pool/set", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"Pool Setpoint\",\"device_class\":\"temperature\","
             "\"icon\": \"mdi:thermometer\","
             "\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"unit_of_measurement\":\"°C\",\"min\":10,\"max\":40,\"step\":1,\"mode\": \"box\","
             "\"value_template\":\"{{ value_json.pool_sp }}\","
             "\"unique_id\":\"%s_pool_sp\",\"availability_topic\":\"%s\",%s}",
             state_topic, command_topic, device_id, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing pool setpoint discovery: %s", config);
    publish_discovery("number", "pool_setpoint", config);
}

// ======================================================
// Spa Setpoint Number Discovery
// ======================================================

static void publish_spa_setpoint_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/temperature/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/temperature/spa/set", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"Spa Setpoint\",\"device_class\":\"temperature\","
             "\"icon\": \"mdi:thermometer\","
             "\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"unit_of_measurement\":\"°C\",\"min\":10,\"max\":40,\"step\":1,\"mode\": \"box\","
             "\"value_template\":\"{{ value_json.spa_sp }}\","
             "\"unique_id\":\"%s_spa_sp\",\"availability_topic\":\"%s\",%s}",
             state_topic, command_topic, device_id, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing spa setpoint discovery: %s", config);
    publish_discovery("number", "spa_setpoint", config);
}

// ======================================================
// Heater Switch Discovery
// ======================================================

static void publish_heater_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/heater/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/heater/set", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"Heater\",\"icon\":\"mdi:radiator\","
             "\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"unique_id\":\"%s_heater\",\"availability_topic\":\"%s\",%s}",
             state_topic, command_topic, device_id, avail_topic, device_json);

    publish_discovery("switch", "heater", config);
}

// ======================================================
// Mode Select Discovery
// ======================================================

static void publish_mode_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/mode/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/mode/set", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"Mode\",\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"options\":[\"Pool\",\"Spa\"],"
             "\"unique_id\":\"%s_mode\",\"availability_topic\":\"%s\",%s}",
             state_topic, command_topic, device_id, avail_topic, device_json);

    publish_discovery("select", "mode", config);
}

// ======================================================
// Channel Switch Discovery (8 channels)
// ======================================================

static void publish_channel_discovery(const char *device_id, int channel_num, const char *channel_name)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/channel/%d/state", device_id, channel_num);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/channel/%d/set", device_id, channel_num);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char object_id[32];
    snprintf(object_id, sizeof(object_id), "channel_%d", channel_num);

    // Format name with channel number prefix: "Ch1 - Filter"
    char formatted_name[64];
    snprintf(formatted_name, sizeof(formatted_name), "Ch%d - %s", channel_num, channel_name);

    // Allocate config buffer on heap to avoid stack overflow
    char *config = malloc(MQTT_DISCOVERY_CONFIG_SIZE);
    if (!config) {
        ESP_LOGE(TAG, "Failed to allocate memory for channel discovery config");
        return;
    }

    snprintf(config, MQTT_DISCOVERY_CONFIG_SIZE,
             "{\"name\":\"%s\",\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"value_template\":\"{%% if value_json.state == 'On' %%}ON{%% else %%}OFF{%% endif %%}\","
             "\"unique_id\":\"%s_ch%d\",\"availability_topic\":\"%s\",%s}",
             formatted_name, state_topic, command_topic, device_id, channel_num, avail_topic, device_json);

    publish_discovery("switch", object_id, config);
    free(config);
}

// ======================================================
// Light Discovery (4 zones)
// ======================================================

static void publish_light_discovery(const char *device_id, int zone_num)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/light/%d/state", device_id, zone_num);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/light/%d/set", device_id, zone_num);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char object_id[32];
    snprintf(object_id, sizeof(object_id), "light_%d", zone_num);

    // Allocate config buffer on heap to avoid stack overflow
    char *config = malloc(MQTT_DISCOVERY_CONFIG_SIZE);
    if (!config) {
        ESP_LOGE(TAG, "Failed to allocate memory for light discovery config");
        return;
    }

    snprintf(config, MQTT_DISCOVERY_CONFIG_SIZE,
             "{\"name\":\"Light Zone %d\",\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"state_value_template\":\"{%% if value_json.state == 'On' %%}ON{%% else %%}OFF{%% endif %%}\","
             "\"unique_id\":\"%s_light%d\",\"availability_topic\":\"%s\",%s}",
             zone_num, state_topic, command_topic, device_id, zone_num, avail_topic, device_json);

    publish_discovery("light", object_id, config);
    free(config);
}

// ======================================================
// pH Sensor Discovery
// ======================================================

static void publish_ph_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"pH Level\","
             "\"state_topic\":\"%s\","
             "\"state_class\":\"measurement\","
             "\"value_template\":\"{{ value_json.ph }}\","
             "\"unit_of_measurement\":\"pH\","
             "\"icon\":\"mdi:ph\","
             "\"unique_id\":\"%s_ph\",\"availability_topic\":\"%s\",%s}",
             state_topic, device_id, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing pH discovery: %s", config);
    publish_discovery("sensor", "ph_level", config);
}

// ======================================================
// ORP Sensor Discovery
// ======================================================

static void publish_orp_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"ORP Level\",\"device_class\":\"voltage\","
             "\"state_topic\":\"%s\","
             "\"state_class\":\"measurement\","
             "\"value_template\":\"{{ value_json.orp }}\","
             "\"unit_of_measurement\":\"mV\","
             "\"unique_id\":\"%s_orp\",\"availability_topic\":\"%s\",%s}",
             state_topic, device_id, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing ORP discovery: %s", config);
    publish_discovery("sensor", "orp", config);
}

// ======================================================
// pH Setpoint Sensor Discovery
// ======================================================

static void publish_ph_setpoint_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"pH Setpoint\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.ph_setpoint }}\","
             "\"unit_of_measurement\":\"pH\","
             "\"icon\":\"mdi:ph\","
             "\"unique_id\":\"%s_ph_setpoint\",\"availability_topic\":\"%s\",%s}",
             state_topic, device_id, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing pH setpoint discovery: %s", config);
    publish_discovery("sensor", "ph_setpoint", config);
}

// ======================================================
// ORP Setpoint Sensor Discovery
// ======================================================

static void publish_orp_setpoint_discovery(const char *device_id)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char device_json[256];
    build_device_json(device_json, sizeof(device_json), device_id);

    char config[1024];
    snprintf(config, sizeof(config),
             "{\"name\":\"ORP Setpoint\",\"device_class\":\"voltage\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.orp_setpoint }}\","
             "\"unit_of_measurement\":\"mV\","
             "\"unique_id\":\"%s_orp_setpoint\",\"availability_topic\":\"%s\",%s}",
             state_topic, device_id, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing ORP setpoint discovery: %s", config);
    publish_discovery("sensor", "orp_setpoint", config);
}

// ======================================================
// Individual Discovery Functions (called when items first configured)
// ======================================================

void mqtt_publish_channel_discovery_single(int channel_num, const char *channel_name)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    ESP_LOGI(TAG, "Publishing discovery for channel %d: %s", channel_num, channel_name);
    publish_channel_discovery(device_id, channel_num, channel_name);
}

void mqtt_publish_light_discovery_single(int zone_num)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    ESP_LOGI(TAG, "Publishing discovery for light zone %d", zone_num);
    publish_light_discovery(device_id, zone_num);
}

// ======================================================
// Main Discovery Function
// ======================================================

void mqtt_publish_discovery(void)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    ESP_LOGI(TAG, "Publishing Home Assistant discovery messages for device: %s", device_id);

    // Temperature and setpoints
    publish_temperature_discovery(device_id);
    publish_pool_setpoint_discovery(device_id);
    publish_spa_setpoint_discovery(device_id);

    // Heater
    publish_heater_discovery(device_id);

    // Mode
    publish_mode_discovery(device_id);

    // Note: Channels and lights are NOT published here.
    // They are published individually when first configured (see mqtt_publish.c)

    // Chemistry
    publish_ph_discovery(device_id);
    publish_orp_discovery(device_id);
    publish_ph_setpoint_discovery(device_id);
    publish_orp_setpoint_discovery(device_id);

    ESP_LOGI(TAG, "Discovery messages published");
}
