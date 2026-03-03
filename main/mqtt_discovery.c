#include "mqtt_discovery.h"
#include "config.h"
#include "mqtt_poolclient.h"
#include "pool_state.h"
#include "device_serial.h"
#include "wifi_provisioning.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "MQTT_DISCOVERY";

#define DISCOVERY_ID_PREFIX "pool_controller"

// Lowercase + spaces-to-underscores, for use in unique_id/object_id
static void normalize_name(const char *in, char *out, size_t out_len)
{
    size_t i = 0;
    for (; i < out_len - 1 && in[i] != '\0'; i++) {
        out[i] = (in[i] == ' ') ? '_' : tolower((unsigned char)in[i]);
    }
    out[i] = '\0';
}

// Helper function to build device JSON
static void build_device_json(char *device_json, size_t max_len, const char *device_id, const char *mac_suffix)
{
    char serial[DEVICE_SERIAL_LEN];
    device_get_serial(serial, sizeof(serial));

    char mac_str[DEVICE_MAC_STRING_LEN];
    device_get_mac_string(mac_str, sizeof(mac_str));

    const esp_app_desc_t *app = esp_app_get_description();
    const char *hostname = wifi_get_mdns_hostname();

    snprintf(device_json, max_len,
             "\"device\":{"
             "\"identifiers\":[\"%s\"],"
             "\"connections\":[[\"mac\",\"%s\"]],"
             "\"name\":\"Pool Controller %s\","
             "\"model\":\"" DEVICE_MODEL "\","
             "\"manufacturer\":\"" DEVICE_MANUFACTURER "\","
             "\"serial_number\":\"%s\","
             "\"sw_version\":\"%s\","
             "\"hw_version\":\"ESP32-C6\","
             "\"configuration_url\":\"http://%s.local\","
             "\"suggested_area\":\"Pool\""
             "}",
             device_id, mac_str, mac_suffix,
             serial, app->version, hostname);
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

static void publish_temperature_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/temperature/state", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_temperature", mac_suffix);

    char config[1152];
    snprintf(config, sizeof(config),
             "{\"name\":\"Temperature\",\"device_class\":\"temperature\","
             "\"icon\":\"mdi:thermometer\","
             "\"state_topic\":\"%s\",\"unit_of_measurement\":\"°C\","
             "\"value_template\":\"{{ value_json.current }}\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"sensor.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, uid, uid, avail_topic, device_json);

    publish_discovery("sensor", uid, config);
}

// ======================================================
// Pool Setpoint Number Discovery
// ======================================================

static void publish_pool_setpoint_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/temperature/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/temperature/pool/set", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_pool_setpoint", mac_suffix);

    char config[1408];
    snprintf(config, sizeof(config),
             "{\"name\":\"Pool Setpoint\",\"device_class\":\"temperature\","
             "\"icon\":\"mdi:thermometer\","
             "\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"unit_of_measurement\":\"°C\",\"min\":10,\"max\":40,\"step\":1,\"mode\":\"box\","
             "\"value_template\":\"{{ value_json.pool_sp }}\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"number.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, command_topic, uid, uid, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing pool setpoint discovery: %s", config);
    publish_discovery("number", uid, config);
}

// ======================================================
// Spa Setpoint Number Discovery
// ======================================================

static void publish_spa_setpoint_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/temperature/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/temperature/spa/set", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_spa_setpoint", mac_suffix);

    char config[1408];
    snprintf(config, sizeof(config),
             "{\"name\":\"Spa Setpoint\",\"device_class\":\"temperature\","
             "\"icon\":\"mdi:thermometer\","
             "\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"unit_of_measurement\":\"°C\",\"min\":10,\"max\":40,\"step\":1,\"mode\":\"box\","
             "\"value_template\":\"{{ value_json.spa_sp }}\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"number.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, command_topic, uid, uid, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing spa setpoint discovery: %s", config);
    publish_discovery("number", uid, config);
}

// ======================================================
// Heater Switch Discovery
// ======================================================

static void publish_heater_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/heater/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/heater/set", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater", mac_suffix);

    char config[1280];
    snprintf(config, sizeof(config),
             "{\"name\":\"Heater\",\"icon\":\"mdi:radiator\","
             "\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"switch.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, command_topic, uid, uid, avail_topic, device_json);

    publish_discovery("switch", uid, config);
}

// ======================================================
// Mode Select Discovery
// ======================================================

static void publish_mode_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/mode/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/mode/set", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_mode", mac_suffix);

    char config[1280];
    snprintf(config, sizeof(config),
             "{\"name\":\"Mode\",\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"options\":[\"Pool\",\"Spa\"],"
             "\"unique_id\":\"%s\",\"default_entity_id\":\"select.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, command_topic, uid, uid, avail_topic, device_json);

    publish_discovery("select", uid, config);
}

// ======================================================
// Channel Switch Discovery (8 channels)
// ======================================================

static void publish_channel_discovery(const char *device_id, const char *mac_suffix,
                                      int channel_num, const char *channel_name)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/channel/%d/state", device_id, channel_num);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/channel/%d/set", device_id, channel_num);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    // Determine display name and normalized part for IDs
    char display_name[64];
    char norm_name[32];
    if (channel_name && channel_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s", channel_name);
        normalize_name(channel_name, norm_name, sizeof(norm_name));
    } else {
        snprintf(display_name, sizeof(display_name), "Channel %d", channel_num);
        norm_name[0] = '\0';
    }

    // Build unique IDs
    char sensor_uid[64];
    char button_uid[64];
    if (norm_name[0] != '\0') {
        snprintf(sensor_uid, sizeof(sensor_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d_%s", mac_suffix, channel_num, norm_name);
        snprintf(button_uid, sizeof(button_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d_%s_toggle", mac_suffix, channel_num, norm_name);
    } else {
        snprintf(sensor_uid, sizeof(sensor_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d", mac_suffix, channel_num);
        snprintf(button_uid, sizeof(button_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d_toggle", mac_suffix, channel_num);
    }

    // Allocate config buffer on heap to avoid stack overflow
    char *config = malloc(MQTT_DISCOVERY_CONFIG_SIZE);
    if (!config) {
        ESP_LOGE(TAG, "Failed to allocate memory for channel discovery config");
        return;
    }

    // Sensor for state
    snprintf(config, MQTT_DISCOVERY_CONFIG_SIZE,
             "{\"name\":\"%s\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.state }}\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"sensor.%s\","
             "\"availability_topic\":\"%s\",%s}",
             display_name, state_topic, sensor_uid, sensor_uid, avail_topic, device_json);
    publish_discovery("sensor", sensor_uid, config);

    // Button for toggle
    snprintf(config, MQTT_DISCOVERY_CONFIG_SIZE,
             "{\"name\":\"%s\",\"command_topic\":\"%s\","
             "\"payload_press\":\"TOGGLE\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"button.%s\","
             "\"availability_topic\":\"%s\",%s}",
             display_name, command_topic, button_uid, button_uid, avail_topic, device_json);
    publish_discovery("button", button_uid, config);

    free(config);
}

// ======================================================
// Light Discovery (4 zones)
// ======================================================

static void publish_light_discovery(const char *device_id, const char *mac_suffix,
                                     int zone_num, const char *zone_name)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/light/%d/state", device_id, zone_num);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/light/%d/set", device_id, zone_num);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_light%d", mac_suffix, zone_num);

    char display_name[48];
    if (zone_name && zone_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s", zone_name);
    } else {
        snprintf(display_name, sizeof(display_name), "Light Zone %d", zone_num);
    }

    // Allocate config buffer on heap to avoid stack overflow
    char *config = malloc(MQTT_DISCOVERY_CONFIG_SIZE);
    if (!config) {
        ESP_LOGE(TAG, "Failed to allocate memory for light discovery config");
        return;
    }

    snprintf(config, MQTT_DISCOVERY_CONFIG_SIZE,
             "{\"name\":\"%s\",\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"state_value_template\":\"{%% if value_json.state == 'On' %%}ON{%% else %%}OFF{%% endif %%}\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"light.%s\","
             "\"availability_topic\":\"%s\",%s}",
             display_name, state_topic, command_topic, uid, uid, avail_topic, device_json);

    publish_discovery("light", uid, config);
    free(config);
}

// ======================================================
// pH Sensor Discovery
// ======================================================

static void publish_ph_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_ph", mac_suffix);

    char config[1152];
    snprintf(config, sizeof(config),
             "{\"name\":\"pH\","
             "\"device_class\":\"ph\","
             "\"state_topic\":\"%s\","
             "\"state_class\":\"measurement\","
             "\"value_template\":\"{{ value_json.ph }}\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"sensor.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, uid, uid, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing pH discovery: %s", config);
    publish_discovery("sensor", uid, config);
}

// ======================================================
// ORP Sensor Discovery
// ======================================================

static void publish_orp_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_orp", mac_suffix);

    char config[1152];
    snprintf(config, sizeof(config),
             "{\"name\":\"ORP\",\"device_class\":\"voltage\","
             "\"state_topic\":\"%s\","
             "\"state_class\":\"measurement\","
             "\"value_template\":\"{{ value_json.orp }}\","
             "\"unit_of_measurement\":\"mV\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"sensor.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, uid, uid, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing ORP discovery: %s", config);
    publish_discovery("sensor", uid, config);
}

// ======================================================
// pH Setpoint Sensor Discovery
// ======================================================

static void publish_ph_setpoint_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_ph_setpoint", mac_suffix);

    char config[1152];
    snprintf(config, sizeof(config),
             "{\"name\":\"pH Setpoint\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.ph_setpoint }}\","
             "\"unit_of_measurement\":\"pH\","
             "\"icon\":\"mdi:ph\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"sensor.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, uid, uid, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing pH setpoint discovery: %s", config);
    publish_discovery("sensor", uid, config);
}

// ======================================================
// ORP Setpoint Sensor Discovery
// ======================================================

static void publish_orp_setpoint_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_orp_setpoint", mac_suffix);

    char config[1152];
    snprintf(config, sizeof(config),
             "{\"name\":\"ORP Setpoint\",\"device_class\":\"voltage\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.orp_setpoint }}\","
             "\"unit_of_measurement\":\"mV\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"sensor.%s\","
             "\"availability_topic\":\"%s\",%s}",
             state_topic, uid, uid, avail_topic, device_json);

    ESP_LOGI(TAG, "Publishing ORP setpoint discovery: %s", config);
    publish_discovery("sensor", uid, config);
}

// ======================================================
// Individual Discovery Functions (called when items first configured)
// ======================================================

void mqtt_publish_channel_discovery_single(int channel_num, const char *channel_name)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing discovery for channel %d: %s", channel_num, channel_name);
    publish_channel_discovery(device_id, mac_suffix, channel_num, channel_name);
}

void mqtt_publish_light_discovery_single(int zone_num, const char *zone_name)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing discovery for light zone %d: %s", zone_num, zone_name ? zone_name : "(unnamed)");
    publish_light_discovery(device_id, mac_suffix, zone_num, zone_name);
}

// ======================================================
// Valve Sensor Discovery
// ======================================================

static void publish_valve_discovery(const char *device_id, const char *mac_suffix,
                                    int valve_num, const char *valve_name)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/valve/%d/state", device_id, valve_num);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/valve/%d/set", device_id, valve_num);

    char device_json[512];
    build_device_json(device_json, sizeof(device_json), device_id, mac_suffix);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_valve%d", mac_suffix, valve_num);

    char display_name[48];
    if (valve_name && valve_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s", valve_name);
    } else {
        snprintf(display_name, sizeof(display_name), "Valve %d", valve_num);
    }

    char *config = malloc(MQTT_DISCOVERY_CONFIG_SIZE);
    if (!config) {
        ESP_LOGE(TAG, "Failed to allocate memory for valve discovery config");
        return;
    }

    snprintf(config, MQTT_DISCOVERY_CONFIG_SIZE,
             "{\"name\":\"%s\","
             "\"state_topic\":\"%s\",\"command_topic\":\"%s\","
             "\"options\":[\"Off\",\"Auto\",\"On\"],"
             "\"value_template\":\"{{ value_json.state }}\","
             "\"unique_id\":\"%s\",\"default_entity_id\":\"select.%s\","
             "\"availability_topic\":\"%s\",%s}",
             display_name, state_topic, command_topic, uid, uid, avail_topic, device_json);

    publish_discovery("select", uid, config);
    free(config);
}

void mqtt_publish_valve_discovery_single(int valve_num, const char *valve_name)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing discovery for valve %d: %s", valve_num, valve_name ? valve_name : "(unnamed)");
    publish_valve_discovery(device_id, mac_suffix, valve_num, valve_name);
}

// ======================================================
// Main Discovery Function
// ======================================================

void mqtt_publish_discovery(void)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing Home Assistant discovery messages for device: %s", device_id);

    // Temperature and setpoints
    publish_temperature_discovery(device_id, mac_suffix);
    publish_pool_setpoint_discovery(device_id, mac_suffix);
    publish_spa_setpoint_discovery(device_id, mac_suffix);

    // Heater
    publish_heater_discovery(device_id, mac_suffix);

    // Mode
    publish_mode_discovery(device_id, mac_suffix);

    // Note: Channels and lights are NOT published here.
    // They are published individually when first configured (see mqtt_publish.c)

    // Chemistry
    publish_ph_discovery(device_id, mac_suffix);
    publish_orp_discovery(device_id, mac_suffix);
    publish_ph_setpoint_discovery(device_id, mac_suffix);
    publish_orp_setpoint_discovery(device_id, mac_suffix);

    ESP_LOGI(TAG, "Discovery messages published");
}
