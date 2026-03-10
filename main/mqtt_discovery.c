#include "mqtt_discovery.h"
#include "config.h"
#include "mqtt_poolclient.h"
#include "pool_state.h"
#include "device_serial.h"
#include "wifi_provisioning.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
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

// Build a device cJSON object (caller must not free separately — embed in root)
static cJSON *build_device_cjson(const char *device_id, const char *mac_suffix)
{
    char serial[DEVICE_SERIAL_LEN];
    device_get_serial(serial, sizeof(serial));

    char mac_str[DEVICE_MAC_STRING_LEN];
    device_get_mac_string(mac_str, sizeof(mac_str));

    const esp_app_desc_t *app = esp_app_get_description();
    const char *hostname = wifi_get_mdns_hostname();

    cJSON *device = cJSON_CreateObject();

    // identifiers: ["<device_id>"]
    cJSON *identifiers = cJSON_CreateArray();
    cJSON_AddItemToArray(identifiers, cJSON_CreateString(device_id));
    cJSON_AddItemToObject(device, "identifiers", identifiers);

    // connections: [["mac", "<mac_str>"]]
    cJSON *connections = cJSON_CreateArray();
    cJSON *mac_pair = cJSON_CreateArray();
    cJSON_AddItemToArray(mac_pair, cJSON_CreateString("mac"));
    cJSON_AddItemToArray(mac_pair, cJSON_CreateString(mac_str));
    cJSON_AddItemToArray(connections, mac_pair);
    cJSON_AddItemToObject(device, "connections", connections);

    char name_buf[48];
    snprintf(name_buf, sizeof(name_buf), "Pool Controller %s", mac_suffix);
    cJSON_AddStringToObject(device, "name", name_buf);
    cJSON_AddStringToObject(device, "model", DEVICE_MODEL);
    cJSON_AddStringToObject(device, "manufacturer", DEVICE_MANUFACTURER);
    cJSON_AddStringToObject(device, "serial_number", serial);
    cJSON_AddStringToObject(device, "sw_version", app->version);
    cJSON_AddStringToObject(device, "hw_version", "ESP32-C6");

    char config_url[128];
    snprintf(config_url, sizeof(config_url), "http://%s.local", hostname);
    cJSON_AddStringToObject(device, "configuration_url", config_url);
    cJSON_AddStringToObject(device, "suggested_area", "Pool");

    return device;
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_temperature", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Temperature");
    cJSON_AddStringToObject(root, "device_class", "temperature");
    cJSON_AddStringToObject(root, "icon", "mdi:thermometer");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "unit_of_measurement", "°C");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.current }}");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print temperature discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_pool_setpoint", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Pool Setpoint");
    cJSON_AddStringToObject(root, "device_class", "temperature");
    cJSON_AddStringToObject(root, "icon", "mdi:thermometer");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "unit_of_measurement", "°C");
    cJSON_AddNumberToObject(root, "min", TEMP_SETPOINT_MIN_C);
    cJSON_AddNumberToObject(root, "max", TEMP_SETPOINT_MAX_C);
    cJSON_AddNumberToObject(root, "step", 1);
    cJSON_AddStringToObject(root, "mode", "box");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.pool_sp }}");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print pool setpoint discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing pool setpoint discovery: %s", json_str);
    publish_discovery("number", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_spa_setpoint", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Spa Setpoint");
    cJSON_AddStringToObject(root, "device_class", "temperature");
    cJSON_AddStringToObject(root, "icon", "mdi:thermometer");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "unit_of_measurement", "°C");
    cJSON_AddNumberToObject(root, "min", TEMP_SETPOINT_MIN_C);
    cJSON_AddNumberToObject(root, "max", TEMP_SETPOINT_MAX_C);
    cJSON_AddNumberToObject(root, "step", 1);
    cJSON_AddStringToObject(root, "mode", "box");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.spa_sp }}");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print spa setpoint discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing spa setpoint discovery: %s", json_str);
    publish_discovery("number", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

// ======================================================
// Heater Switch Discovery
// ======================================================

static void publish_heater_discovery(const char *device_id, const char *mac_suffix, int index)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/heater/%d/state", device_id, index);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/heater/%d/set", device_id, index);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d", mac_suffix, index);

    char display_name[32];
    snprintf(display_name, sizeof(display_name), "Heater %d", index + 1);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", display_name);
    cJSON_AddStringToObject(root, "icon", "mdi:radiator");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print heater discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("switch", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void mqtt_publish_heater_discovery_single(int index)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing discovery for heater %d", index);
    publish_heater_discovery(device_id, mac_suffix, index);
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_mode", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Mode");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);

    cJSON *opts = cJSON_CreateArray();
    cJSON_AddItemToArray(opts, cJSON_CreateString("Pool"));
    cJSON_AddItemToArray(opts, cJSON_CreateString("Spa"));
    cJSON_AddItemToObject(root, "options", opts);

    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print mode discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("select", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    // Sensor for state
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.state }}");
        cJSON_AddStringToObject(root, "unique_id", sensor_uid);
        cJSON_AddStringToObject(root, "default_entity_id", sensor_uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print channel sensor discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("sensor", sensor_uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    }

    // Button for toggle
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "command_topic", command_topic);
        cJSON_AddStringToObject(root, "payload_press", "TOGGLE");
        cJSON_AddStringToObject(root, "unique_id", button_uid);
        cJSON_AddStringToObject(root, "default_entity_id", button_uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print channel button discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("button", button_uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    }
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_light%d", mac_suffix, zone_num);

    char display_name[48];
    if (zone_name && zone_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s", zone_name);
    } else {
        snprintf(display_name, sizeof(display_name), "Light Zone %d", zone_num);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", display_name);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "state_value_template",
                            "{% if value_json.state == 'On' %}ON{% else %}OFF{% endif %}");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print light discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("light", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_ph", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "pH");
    cJSON_AddStringToObject(root, "device_class", "ph");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "state_class", "measurement");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.ph }}");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print pH discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing pH discovery: %s", json_str);
    publish_discovery("sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_orp", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "ORP");
    cJSON_AddStringToObject(root, "device_class", "voltage");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "state_class", "measurement");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.orp }}");
    cJSON_AddStringToObject(root, "unit_of_measurement", "mV");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print ORP discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing ORP discovery: %s", json_str);
    publish_discovery("sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_ph_setpoint", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "pH Setpoint");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.ph_setpoint }}");
    cJSON_AddStringToObject(root, "unit_of_measurement", "pH");
    cJSON_AddStringToObject(root, "icon", "mdi:ph");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print pH setpoint discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing pH setpoint discovery: %s", json_str);
    publish_discovery("sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_orp_setpoint", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "ORP Setpoint");
    cJSON_AddStringToObject(root, "device_class", "voltage");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.orp_setpoint }}");
    cJSON_AddStringToObject(root, "unit_of_measurement", "mV");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print ORP setpoint discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing ORP setpoint discovery: %s", json_str);
    publish_discovery("sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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
// Valve Select Discovery
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

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_valve%d", mac_suffix, valve_num);

    char display_name[48];
    if (valve_name && valve_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s", valve_name);
    } else {
        snprintf(display_name, sizeof(display_name), "Valve %d", valve_num);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", display_name);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);

    cJSON *opts = cJSON_CreateArray();
    cJSON_AddItemToArray(opts, cJSON_CreateString("Off"));
    cJSON_AddItemToArray(opts, cJSON_CreateString("Auto"));
    cJSON_AddItemToArray(opts, cJSON_CreateString("On"));
    cJSON_AddItemToObject(root, "options", opts);

    cJSON_AddStringToObject(root, "value_template", "{{ value_json.state }}");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "default_entity_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print valve discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("select", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
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

    // Note: Heaters are NOT published here.
    // They are published individually when first seen (see mqtt_publish.c)

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
