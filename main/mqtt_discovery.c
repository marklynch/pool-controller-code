#include "mqtt_discovery.h"
#include "config.h"
#include "mqtt_poolclient.h"
#include "message_decoder.h"
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
// Per-Source Temperature Sensor Discovery
// ======================================================

static void publish_temperature_sensor_discovery(
    const char *device_id, const char *mac_suffix,
    uint8_t addr_hi, uint8_t addr_lo,
    uint8_t sensor_index, bool single_sensor_source)
{
    char slug[24];
    get_device_slug(addr_hi, addr_lo, slug, sizeof(slug));

    char avail_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);

    char state_topic[160];
    if (single_sensor_source) {
        snprintf(state_topic, sizeof(state_topic), "pool/%s/temperature/%s/state", device_id, slug);
    } else {
        snprintf(state_topic, sizeof(state_topic), "pool/%s/temperature/%s/%u/state",
                 device_id, slug, sensor_index);
    }

    char name_buf[16];
    const char *dev_name = get_device_name(addr_hi, addr_lo, name_buf, sizeof(name_buf));

    char display_name[48];
    if (single_sensor_source) {
        snprintf(display_name, sizeof(display_name), "Temp - %s", dev_name);
    } else {
        snprintf(display_name, sizeof(display_name), "Temp %u - %s", sensor_index, dev_name);
    }

    char uid[96];
    char object_id[96];
    if (single_sensor_source) {
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_temp_%s", mac_suffix, slug);
        snprintf(object_id, sizeof(object_id), DISCOVERY_ID_PREFIX "_%s_%s_temp",
                 mac_suffix, slug);
    } else {
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_temp_%s_%u",
                 mac_suffix, slug, sensor_index);
        snprintf(object_id, sizeof(object_id), DISCOVERY_ID_PREFIX "_%s_%s_temp_%u",
                 mac_suffix, slug, sensor_index);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", display_name);
    cJSON_AddStringToObject(root, "device_class", "temperature");
    cJSON_AddStringToObject(root, "icon", "mdi:thermometer");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "unit_of_measurement", "°C");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.value }}");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", object_id);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print temperature sensor discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void mqtt_publish_temperature_sensor_discovery_single(
    uint8_t addr_hi, uint8_t addr_lo,
    uint8_t sensor_index, bool single_sensor_source)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing temperature sensor discovery for 0x%02X%02X sensor %u",
             addr_hi, addr_lo, sensor_index);
    publish_temperature_sensor_discovery(device_id, mac_suffix,
                                         addr_hi, addr_lo,
                                         sensor_index, single_sensor_source);
}

// ======================================================
// Per-Heater Setpoint Number Discovery
// ======================================================

// Publish one Number entity for a heater's pool or spa setpoint.
static void publish_heater_setpoint_number(const char *device_id, const char *mac_suffix,
                                           int index, bool is_pool)
{
    const char *circuit = is_pool ? "pool" : "spa";
    const char *value_key = is_pool ? "pool_sp" : "spa_sp";

    char avail_topic[128];
    char state_topic[128];
    char command_topic[160];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/heater/%d/setpoints/state", device_id, index);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/heater/%d/%s_setpoint/set", device_id, index, circuit);

    char uid[80];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d_%s_setpoint", mac_suffix, index, circuit);

    char display_name[40];
    snprintf(display_name, sizeof(display_name), "Heater %d %s Setpoint", index + 1, is_pool ? "Pool" : "Spa");

    char value_template[48];
    snprintf(value_template, sizeof(value_template), "{{ value_json.%s }}", value_key);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", display_name);
    cJSON_AddStringToObject(root, "device_class", "temperature");
    cJSON_AddStringToObject(root, "icon", "mdi:thermometer");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "unit_of_measurement", "°C");
    cJSON_AddNumberToObject(root, "min", TEMP_SETPOINT_MIN_C);
    cJSON_AddNumberToObject(root, "max", TEMP_SETPOINT_MAX_C);
    cJSON_AddNumberToObject(root, "step", 1);
    cJSON_AddStringToObject(root, "mode", "box");
    cJSON_AddStringToObject(root, "value_template", value_template);
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print heater %d %s setpoint discovery JSON", index, circuit);
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing heater %d %s setpoint discovery: %s", index, circuit, json_str);
    publish_discovery("number", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void mqtt_publish_heater_setpoint_discovery_single(int index)
{
    if (index < 0 || index >= MAX_HEATERS) return;

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    publish_heater_setpoint_number(device_id, mac_suffix, index, true);   // Pool
    publish_heater_setpoint_number(device_id, mac_suffix, index, false);  // Spa
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
    cJSON_AddStringToObject(root, "object_id", uid);
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
// Gas Heater Detail Discovery
// ======================================================

static void publish_gas_heater_detail_discovery(const char *device_id, const char *mac_suffix, int index)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/heater/%d/gas_status/state", device_id, index);

    char display_name[48];
    char uid[72];

    // Status sensor — the gas_heater_status_t string (off/heating/igniting/…)
    snprintf(display_name, sizeof(display_name), "Heater %d Status", index + 1);
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d_gas_status", mac_suffix, index);
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "device_class", "enum");
        cJSON_AddStringToObject(root, "icon", "mdi:thermometer-lines");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.status }}");
        cJSON *status_opts = cJSON_CreateArray();
        for (int i = 0; i < HEATER_STATUS_NAME_COUNT; i++) {
            cJSON_AddItemToArray(status_opts, cJSON_CreateString(HEATER_STATUS_NAMES[i]));
        }
        cJSON_AddItemToObject(root, "options", status_opts);
        cJSON_AddStringToObject(root, "unique_id", uid);
        cJSON_AddStringToObject(root, "object_id", uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) { publish_discovery("sensor", uid, json_str); cJSON_free(json_str); }
        cJSON_Delete(root);
    }

    // Water flow binary sensor
    snprintf(display_name, sizeof(display_name), "Heater %d Water Flow", index + 1);
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d_water_flow", mac_suffix, index);
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "device_class", "running");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ 'ON' if value_json.water_flow else 'OFF' }}");
        cJSON_AddStringToObject(root, "unique_id", uid);
        cJSON_AddStringToObject(root, "object_id", uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) { publish_discovery("binary_sensor", uid, json_str); cJSON_free(json_str); }
        cJSON_Delete(root);
    }

    // Locked out binary sensor
    snprintf(display_name, sizeof(display_name), "Heater %d Locked Out", index + 1);
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d_locked_out", mac_suffix, index);
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ 'ON' if value_json.locked_out else 'OFF' }}");
        cJSON_AddStringToObject(root, "unique_id", uid);
        cJSON_AddStringToObject(root, "object_id", uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) { publish_discovery("binary_sensor", uid, json_str); cJSON_free(json_str); }
        cJSON_Delete(root);
    }

    // Burner state sensor (off/igniting/alight)
    snprintf(display_name, sizeof(display_name), "Heater %d Burner", index + 1);
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d_burner", mac_suffix, index);
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "device_class", "enum");
        cJSON_AddStringToObject(root, "icon", "mdi:fire");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.burner }}");
        cJSON *burner_opts = cJSON_CreateArray();
        for (int i = 0; i < BURNER_STATE_NAME_COUNT; i++) {
            cJSON_AddItemToArray(burner_opts, cJSON_CreateString(BURNER_STATE_NAMES[i]));
        }
        cJSON_AddItemToObject(root, "options", burner_opts);
        cJSON_AddStringToObject(root, "unique_id", uid);
        cJSON_AddStringToObject(root, "object_id", uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) { publish_discovery("sensor", uid, json_str); cJSON_free(json_str); }
        cJSON_Delete(root);
    }

    // General service required binary sensor
    snprintf(display_name, sizeof(display_name), "Heater %d General Service Required", index + 1);
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d_general_service", mac_suffix, index);
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "device_class", "problem");
        cJSON_AddStringToObject(root, "icon", "mdi:wrench-alert");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ 'ON' if value_json.general_service_required else 'OFF' }}");
        cJSON_AddStringToObject(root, "unique_id", uid);
        cJSON_AddStringToObject(root, "object_id", uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) { publish_discovery("binary_sensor", uid, json_str); cJSON_free(json_str); }
        cJSON_Delete(root);
    }

    // Ignition service required binary sensor
    snprintf(display_name, sizeof(display_name), "Heater %d Ignition Service Required", index + 1);
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d_ignition_service", mac_suffix, index);
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "device_class", "problem");
        cJSON_AddStringToObject(root, "icon", "mdi:fire-alert");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ 'ON' if value_json.ignition_service_required else 'OFF' }}");
        cJSON_AddStringToObject(root, "unique_id", uid);
        cJSON_AddStringToObject(root, "object_id", uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) { publish_discovery("binary_sensor", uid, json_str); cJSON_free(json_str); }
        cJSON_Delete(root);
    }

    // Cooling available binary sensor
    snprintf(display_name, sizeof(display_name), "Heater %d Cooling Available", index + 1);
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_heater_%d_cooling_available", mac_suffix, index);
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "icon", "mdi:snowflake");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ 'ON' if value_json.cooling_available else 'OFF' }}");
        cJSON_AddStringToObject(root, "unique_id", uid);
        cJSON_AddStringToObject(root, "object_id", uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) { publish_discovery("binary_sensor", uid, json_str); cJSON_free(json_str); }
        cJSON_Delete(root);
    }
}

void mqtt_publish_gas_heater_discovery_single(int index)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing gas heater detail discovery for heater %d", index);
    publish_gas_heater_detail_discovery(device_id, mac_suffix, index);
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
    cJSON_AddStringToObject(root, "object_id", uid);
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
    char active_uid[64];
    if (norm_name[0] != '\0') {
        snprintf(sensor_uid, sizeof(sensor_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d_%s", mac_suffix, channel_num, norm_name);
        snprintf(button_uid, sizeof(button_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d_%s_toggle", mac_suffix, channel_num, norm_name);
        snprintf(active_uid, sizeof(active_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d_%s_active", mac_suffix, channel_num, norm_name);
    } else {
        snprintf(sensor_uid, sizeof(sensor_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d", mac_suffix, channel_num);
        snprintf(button_uid, sizeof(button_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d_toggle", mac_suffix, channel_num);
        snprintf(active_uid, sizeof(active_uid),
                 DISCOVERY_ID_PREFIX "_%s_ch%d_active", mac_suffix, channel_num);
    }

    // Sensor for state
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", display_name);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.state }}");
        cJSON_AddStringToObject(root, "unique_id", sensor_uid);
        cJSON_AddStringToObject(root, "object_id", sensor_uid);
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
        cJSON_AddStringToObject(root, "object_id", button_uid);
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

    // Binary sensor for active state
    {
        char active_name[80];
        snprintf(active_name, sizeof(active_name), "%s Active", display_name);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", active_name);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ 'ON' if value_json.active else 'OFF' }}");
        cJSON_AddStringToObject(root, "unique_id", active_uid);
        cJSON_AddStringToObject(root, "object_id", active_uid);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print channel active binary sensor discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("binary_sensor", active_uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    }
}

// ======================================================
// Light Discovery (4 zones)
// ======================================================

static void publish_light_discovery(const char *device_id, const char *mac_suffix,
                                     int zone_num, const char *zone_name,
                                     uint8_t multicolor_light_type)
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
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);

    // Color selection as an effect list for multicolor zones: the model's
    // subset of the shared color table (register 0xF0 selects the model)
    int color_count = 0;
    const uint8_t *color_codes = multicolor_light_color_codes(multicolor_light_type, &color_count);
    if (color_codes && color_count > 0) {
        char effect_command_topic[128];
        snprintf(effect_command_topic, sizeof(effect_command_topic),
                 "pool/%s/light/%d/color/set", device_id, zone_num);

        cJSON *effect_list = cJSON_CreateArray();
        for (int i = 0; i < color_count; i++) {
            if (color_codes[i] < LIGHTING_COLOR_COUNT) {
                cJSON_AddItemToArray(effect_list, cJSON_CreateString(LIGHTING_COLOR_NAMES[color_codes[i]]));
            }
        }
        cJSON_AddItemToObject(root, "effect_list", effect_list);
        cJSON_AddStringToObject(root, "effect_command_topic", effect_command_topic);
        cJSON_AddStringToObject(root, "effect_state_topic", state_topic);
        cJSON_AddStringToObject(root, "effect_value_template", "{{ value_json.color }}");
    }

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

    // Resync button for multicolor zones: pressing it fires the Light Resync
    // Command (CMD 0x3C) for this zone
    char resync_uid[64];
    snprintf(resync_uid, sizeof(resync_uid), DISCOVERY_ID_PREFIX "_%s_light%d_resync", mac_suffix, zone_num);

    if (color_codes && color_count > 0) {
        char resync_command_topic[128];
        snprintf(resync_command_topic, sizeof(resync_command_topic),
                 "pool/%s/light/%d/resync", device_id, zone_num);

        char resync_name[64];
        snprintf(resync_name, sizeof(resync_name), "%s Resync", display_name);

        cJSON *btn = cJSON_CreateObject();
        cJSON_AddStringToObject(btn, "name", resync_name);
        cJSON_AddStringToObject(btn, "command_topic", resync_command_topic);
        cJSON_AddStringToObject(btn, "icon", "mdi:sync");
        cJSON_AddStringToObject(btn, "unique_id", resync_uid);
        cJSON_AddStringToObject(btn, "object_id", resync_uid);
        cJSON_AddStringToObject(btn, "availability_topic", avail_topic);
        cJSON_AddItemToObject(btn, "device", build_device_cjson(device_id, mac_suffix));

        char *btn_json = cJSON_PrintUnformatted(btn);
        if (!btn_json) {
            ESP_LOGE(TAG, "Failed to print light resync button discovery JSON");
            cJSON_Delete(btn);
            return;
        }
        publish_discovery("button", resync_uid, btn_json);
        cJSON_free(btn_json);
        cJSON_Delete(btn);
    } else {
        // Clear any previously published resync button if the zone is no
        // longer multicolor (an empty retained config removes the HA entity)
        char config_topic[256];
        snprintf(config_topic, sizeof(config_topic),
                 "homeassistant/button/%s/%s/config", device_id, resync_uid);
        mqtt_publish(config_topic, "", 1, true);
    }
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
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "state_class", "measurement");
    cJSON_AddStringToObject(root, "unit_of_measurement", "pH");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.ph }}");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
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
    cJSON_AddStringToObject(root, "object_id", uid);
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
    cJSON_AddStringToObject(root, "object_id", uid);
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
    cJSON_AddStringToObject(root, "object_id", uid);
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
// Chlorine Output Level Sensor Discovery
// ======================================================

static void publish_chlor_output_level_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/chlorinator/state", device_id);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_chlor_output_level", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Chlorine Output Level");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.chlor_output_level }}");
    cJSON_AddStringToObject(root, "icon", "mdi:gauge");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print salt chlor setpoint discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing salt chlor setpoint discovery: %s", json_str);
    publish_discovery("sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

// ======================================================
// Pump Speed Sensor Discovery
// ======================================================

static void publish_pump_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/pump/state", device_id);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_pump_speed", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Pump Speed");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.speed_rpm }}");
    cJSON_AddStringToObject(root, "unit_of_measurement", "RPM");
    cJSON_AddStringToObject(root, "icon", "mdi:pump");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print pump speed discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing pump speed discovery: %s", json_str);
    publish_discovery("sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    // Pump Power Discovery
    char uid_power[64];
    snprintf(uid_power, sizeof(uid_power), DISCOVERY_ID_PREFIX "_%s_pump_power", mac_suffix);

    cJSON *power_root = cJSON_CreateObject();
    cJSON_AddStringToObject(power_root, "name", "Pump Power");
    cJSON_AddStringToObject(power_root, "state_topic", state_topic);
    cJSON_AddStringToObject(power_root, "value_template", "{{ value_json.power_watts }}");
    cJSON_AddStringToObject(power_root, "unit_of_measurement", "W");
    cJSON_AddStringToObject(power_root, "device_class", "power");
    cJSON_AddStringToObject(power_root, "state_class", "measurement");    
    cJSON_AddStringToObject(power_root, "icon", "mdi:flash");
    cJSON_AddStringToObject(power_root, "unique_id", uid_power);
    cJSON_AddStringToObject(power_root, "object_id", uid_power);
    cJSON_AddStringToObject(power_root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(power_root, "device", build_device_cjson(device_id, mac_suffix));

    char *power_json_str = cJSON_PrintUnformatted(power_root);
    if (power_json_str) {
        publish_discovery("sensor", uid_power, power_json_str);
        cJSON_free(power_json_str);
    }
    cJSON_Delete(power_root);
}

static void publish_service_mode_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/service_mode/state", device_id);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_service_mode", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Service Mode");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "icon", "mdi:wrench");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print service mode discovery JSON");
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "Publishing service mode discovery: %s", json_str);
    publish_discovery("binary_sensor", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

// ======================================================
// Individual Discovery Functions (called when items first configured)
// ======================================================

// Fetch device_id/mac_suffix and invoke a no-arg discovery publisher.
static void publish_single(void (*publish_fn)(const char *device_id, const char *mac_suffix))
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    publish_fn(device_id, mac_suffix);
}

void mqtt_publish_ph_discovery_single(void)                { publish_single(publish_ph_discovery); }
void mqtt_publish_orp_discovery_single(void)               { publish_single(publish_orp_discovery); }
void mqtt_publish_ph_setpoint_discovery_single(void)       { publish_single(publish_ph_setpoint_discovery); }
void mqtt_publish_orp_setpoint_discovery_single(void)      { publish_single(publish_orp_setpoint_discovery); }
void mqtt_publish_chlor_output_level_discovery_single(void) { publish_single(publish_chlor_output_level_discovery); }
void mqtt_publish_pump_discovery_single(void)              { publish_single(publish_pump_discovery); }
void mqtt_publish_service_mode_discovery_single(void)      { publish_single(publish_service_mode_discovery); }

void mqtt_publish_channel_discovery_single(int channel_num, const char *channel_name)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing discovery for channel %d: %s", channel_num, channel_name);
    publish_channel_discovery(device_id, mac_suffix, channel_num, channel_name);
}

void mqtt_publish_light_discovery_single(int zone_num, const char *zone_name, uint8_t multicolor_light_type)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing discovery for light zone %d: %s", zone_num, zone_name ? zone_name : "(unnamed)");
    publish_light_discovery(device_id, mac_suffix, zone_num, zone_name, multicolor_light_type);
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
    cJSON_AddStringToObject(root, "object_id", uid);
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
// Favourite Select Discovery
// ======================================================

static void publish_favourite_discovery(const char *device_id, const char *mac_suffix,
                                        const pool_state_t *state)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/favourite/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/favourite/set", device_id);

    char uid[64];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_favourite", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Favourite");
    cJSON_AddStringToObject(root, "icon", "mdi:pool");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);

    cJSON *opts = cJSON_CreateArray();

    // Pool (index FAVOURITE_POOL = 0)
    if (state && state->favourites[FAVOURITE_POOL].name_valid && state->favourites[FAVOURITE_POOL].name[0] != '\0') {
        cJSON_AddItemToArray(opts, cJSON_CreateString(state->favourites[FAVOURITE_POOL].name));
    } else {
        cJSON_AddItemToArray(opts, cJSON_CreateString("Pool"));
    }

    // Spa (index FAVOURITE_SPA = 1)
    if (state && state->favourites[FAVOURITE_SPA].name_valid && state->favourites[FAVOURITE_SPA].name[0] != '\0') {
        cJSON_AddItemToArray(opts, cJSON_CreateString(state->favourites[FAVOURITE_SPA].name));
    } else {
        cJSON_AddItemToArray(opts, cJSON_CreateString("Spa"));
    }

    // User favourites (indices 2–7)
    for (int i = 2; i < MAX_FAVOURITES; i++) {
        if (!state || !state->favourites[i].enabled_valid || !state->favourites[i].enabled) {
            continue;
        }
        if (state->favourites[i].name_valid && state->favourites[i].name[0] != '\0') {
            cJSON_AddItemToArray(opts, cJSON_CreateString(state->favourites[i].name));
        } else {
            char fallback[24];
            snprintf(fallback, sizeof(fallback), "Favourite %d", i - 1);
            cJSON_AddItemToArray(opts, cJSON_CreateString(fallback));
        }
    }

    // All Off / All Auto (always available)
    cJSON_AddItemToArray(opts, cJSON_CreateString("All Off"));
    cJSON_AddItemToArray(opts, cJSON_CreateString("All Auto"));

    // "No Favourite" is status-only (register 0x20 = 0xFF, no favourite
    // active); it is included so HA renders the state as a valid option, but
    // selecting it sends no bus command (see handle_favourite_command).
    // Deliberately not labelled "None" — HA treats that payload as its
    // set-to-unknown sentinel.
    cJSON_AddItemToArray(opts, cJSON_CreateString("No Favourite"));

    cJSON_AddItemToObject(root, "options", opts);
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print favourite discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("select", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void mqtt_publish_favourite_discovery_single(const pool_state_t *state)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing discovery for favourite select");
    publish_favourite_discovery(device_id, mac_suffix, state);
}

// ======================================================
// Main Discovery Function
// ======================================================

// ======================================================
// Firmware Update Entity Discovery
// ======================================================

static void publish_update_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/update/state", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/update/install", device_id);

    char uid[80];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_firmware", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Firmware");
    cJSON_AddStringToObject(root, "device_class", "firmware");
    // State is a JSON payload carrying installed_version/latest_version and
    // in_progress/update_percentage (see firmware_update_publish_mqtt_state).
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "payload_install", "INSTALL");
    // Shown on HA's Updates page and in the update dialog (the mdi `icon`
    // option is ignored there). Fetched by the viewing browser, so it must
    // be a publicly reachable URL.
    cJSON_AddStringToObject(root, "entity_picture",
                            "https://raw.githubusercontent.com/"
                            FW_UPDATE_GITHUB_OWNER "/" FW_UPDATE_GITHUB_REPO
                            "/main/main/static/favicon.png");
    // Installable update entities conventionally live in the Configuration
    // section (matches core UpdateEntity's default, which MQTT doesn't apply).
    cJSON_AddStringToObject(root, "entity_category", "config");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print update discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("update", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

// Button that asks the device to check GitHub for a new release now instead
// of waiting for the periodic check (the HA update platform has no MQTT
// "check" command of its own).
static void publish_update_check_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/update/check", device_id);

    char uid[80];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_fw_check", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Check for firmware update");
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "icon", "mdi:update");
    // Same section as the Firmware update entity it belongs with.
    cJSON_AddStringToObject(root, "entity_category", "config");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print update-check button discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("button", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

// Button that reboots the device, mirroring the web UI's reboot button on
// the firmware-update page.
static void publish_reboot_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/reboot", device_id);

    char uid[80];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_reboot", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Reboot");
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "device_class", "restart");
    cJSON_AddStringToObject(root, "entity_category", "config");
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "object_id", uid);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print reboot button discovery JSON");
        cJSON_Delete(root);
        return;
    }
    publish_discovery("button", uid, json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void mqtt_publish_update_discovery_single(void)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    publish_update_discovery(device_id, mac_suffix);
    publish_update_check_discovery(device_id, mac_suffix);
}

void mqtt_publish_discovery(void)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing Home Assistant discovery messages for device: %s", device_id);

    // Note: Per-heater setpoints are NOT published here.
    // They are published individually on first setpoint reading per heater
    // (see mqtt_publish_heater_setpoints in mqtt_publish.c).

    // Note: Per-source temperature sensors are NOT published here.
    // They are published individually on first CMD 0x16 reading from each
    // (source, sensor) pair (see mqtt_publish.c).

    // Note: Heaters are NOT published here.
    // They are published individually when first seen (see mqtt_publish.c)

    // Mode
    publish_mode_discovery(device_id, mac_suffix);

    // Firmware update entity (GitHub release tracking) + check-now button
    publish_update_discovery(device_id, mac_suffix);
    publish_update_check_discovery(device_id, mac_suffix);

    // Reboot button
    publish_reboot_discovery(device_id, mac_suffix);

    // Note: Channels and lights are NOT published here.
    // They are published individually when first configured (see mqtt_publish.c)

    // Favourite select (snapshot state for dynamic options)
    {
        pool_state_t snapshot = {0};
        if (s_pool_state_mutex &&
            xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            snapshot = s_pool_state;
            xSemaphoreGive(s_pool_state_mutex);
        }
        publish_favourite_discovery(device_id, mac_suffix, &snapshot);
    }

    // Note: Chemistry (pH/ORP readings and setpoints, chlorine output level),
    // pump speed, and service mode are NOT published here. Each entity is
    // published individually on its first valid reading (see
    // mqtt_publish_chlorinator, mqtt_publish_pump, and
    // mqtt_publish_service_mode in mqtt_publish.c), so systems without a
    // chlorinator or variable-speed pump never create the HA entities.

    ESP_LOGI(TAG, "Discovery messages published");
}
