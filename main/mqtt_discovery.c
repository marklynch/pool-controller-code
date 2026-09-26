#include "mqtt_discovery.h"
#include "filter_pump_type.h"
#include "config.h"
#include "mqtt_poolclient.h"
#include "message_decoder.h"
#include "pool_state.h"
#include "device_serial.h"
#include "wifi_provisioning.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MQTT_DISCOVERY";

// Prefix of the unique_id, which is the entity's registry key: it is keyed on
// the MAC suffix and the entity's role only, and must never change for an
// entity that already exists in Home Assistant.
#define DISCOVERY_ID_PREFIX "pool_controller"

// Prefix of the entity_id Home Assistant is asked to use, giving every entity
// the form `<component>.pool_control_<mac>_<role>`.
#define ENTITY_ID_PREFIX "pool_control"

// Add an entity's identity fields.
//
// `unique_id` is the registry key. The entity_id is built separately from
// `slug_fmt` so that it stays consistent across entity types even where the
// unique_id's historic spelling differs (e.g. `ch5` vs `channel_5`).
//
// Home Assistant needs to be told the entity_id explicitly: left to itself it
// derives one from the area, the device name and the entity name (see
// `_async_generate_entity_id`), which both varies with those mutable names and
// repeats the device name we already carry in the id. The key that carries it
// changed in HA 2026.4 — `object_id` before, `default_entity_id` after — so
// publish both, naming the same entity_id either way. Unknown keys are dropped
// by the discovery schema (`extra=vol.REMOVE_EXTRA`), so neither upsets the
// version that doesn't use it.
__attribute__((format(printf, 5, 6)))
static void add_entity_ids(cJSON *root, const char *component, const char *mac_suffix,
                           const char *unique_id, const char *slug_fmt, ...)
{
    char slug[64];
    va_list args;
    va_start(args, slug_fmt);
    vsnprintf(slug, sizeof(slug), slug_fmt, args);
    va_end(args);

    // HA slugifies the entity_id anyway; lowercasing here keeps what we publish
    // identical to what it registers.
    char mac_lower[DEVICE_MAC_SUFFIX_LEN];
    size_t i = 0;
    for (; i < sizeof(mac_lower) - 1 && mac_suffix[i] != '\0'; i++) {
        mac_lower[i] = (char)tolower((unsigned char)mac_suffix[i]);
    }
    mac_lower[i] = '\0';

    char object_id[96];
    snprintf(object_id, sizeof(object_id), ENTITY_ID_PREFIX "_%s_%s", mac_lower, slug);

    char default_entity_id[128];
    snprintf(default_entity_id, sizeof(default_entity_id), "%s.%s", component, object_id);

    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "object_id", object_id);
    cJSON_AddStringToObject(root, "default_entity_id", default_entity_id);
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

// Retract a discovery message: an empty retained payload on the config topic
// tells HA to delete the entity and clears the broker's retained copy, so it
// doesn't come back on the next HA restart. Harmless if nothing was ever
// published there.
static void remove_discovery(const char *component, const char *object_id)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[256];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", component, device_id, object_id);

    mqtt_publish(topic, "", 1, true);
    ESP_LOGI(TAG, "Removed discovery: %s/%s", component, object_id);
}

// ======================================================
// Home Assistant Entity Reset
// ======================================================
//
// Home Assistant assigns an entity_id when it first registers an entity and
// keeps it forever after, so a device whose entities were created under an
// older id scheme never picks up a new one. Clearing the retained discovery
// configs makes HA delete the entities; the fresh configs published on the
// next boot then re-create them under the current scheme (MQTT applies
// `default_entity_id` to an entity restored from a deleted registry entry).
//
// The configs are read back off the broker rather than reconstructed, so
// entities left behind by earlier firmware — published under unique_ids this
// build no longer generates — are cleared too.

#define HA_RESET_TOPIC_FILTER   "homeassistant/+/+/+/config"
#define HA_RESET_COLLECT_MS     3000   // retained configs arrive on subscribe
#define HA_RESET_FLUSH_MS       1000   // let the retractions reach the broker
#define HA_RESET_MAX_TOPICS     128

// Written by the MQTT event handler as configs arrive, read by the reset task
// once it has cleared s_reset_active and no more can be appended.
static atomic_bool s_reset_active = false;
static char *s_reset_topics[HA_RESET_MAX_TOPICS];
static atomic_int s_reset_count = 0;

bool mqtt_discovery_reset_handle_message(const char *topic, int topic_len, int data_len)
{
    if (!atomic_load(&s_reset_active)) {
        return false;
    }

    // Payload continuation of a message whose topic arrived in an earlier
    // event. Swallow it: it belongs to a discovery config, not a command.
    if (topic_len == 0) {
        return true;
    }

    // Only `homeassistant/<component>/<node_id>/<object_id>/config`, and only
    // where <node_id> is ours — everything else on the broker is another
    // device's and must be left alone.
    char buf[256];
    if ((size_t)topic_len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, topic, topic_len);
    buf[topic_len] = '\0';

    char *segments[5];
    int n_segments = 0;
    for (char *p = buf, *next; n_segments < 5; p = next) {
        segments[n_segments++] = p;
        next = strchr(p, '/');
        if (!next) break;
        *next++ = '\0';
    }
    if (n_segments != 5 || strcmp(segments[0], "homeassistant") != 0 ||
        strcmp(segments[4], "config") != 0) {
        return false;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));
    if (strcmp(segments[2], device_id) != 0) {
        return false;
    }

    // Nothing to clear: an empty payload is a config that has already been
    // retracted, including the echo of our own retraction.
    if (data_len == 0) {
        return true;
    }

    int index = atomic_load(&s_reset_count);
    if (index >= HA_RESET_MAX_TOPICS) {
        ESP_LOGW(TAG, "HA entity reset: topic list full, ignoring %s", buf);
        return true;
    }

    // Restore the separators stripped while splitting.
    for (int i = 1; i < 5; i++) {
        segments[i][-1] = '/';
    }

    // Collect only — the retraction is published from the reset task, because
    // a QoS 1 publish from inside the MQTT event handler deadlocks the client.
    char *copy = strdup(buf);
    if (!copy) {
        ESP_LOGE(TAG, "HA entity reset: out of memory for %s", buf);
        return true;
    }
    // Fill the slot before publishing the new count, so the reset task never
    // sees an index whose topic isn't written yet. Safe with a single writer.
    s_reset_topics[index] = copy;
    atomic_store(&s_reset_count, index + 1);
    return true;
}

static void ha_reset_task(void *arg)
{
    ESP_LOGW(TAG, "HA entity reset: collecting retained discovery configs");
    if (mqtt_subscribe(HA_RESET_TOPIC_FILTER, 1) != ESP_OK) {
        // Nothing was cleared, so there's nothing to republish either: leave
        // the device running rather than rebooting it for no reason.
        atomic_store(&s_reset_active, false);
        ESP_LOGE(TAG, "HA entity reset: could not subscribe, aborted");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(HA_RESET_COLLECT_MS));

    atomic_store(&s_reset_active, false);
    mqtt_unsubscribe(HA_RESET_TOPIC_FILTER);

    int count = atomic_load(&s_reset_count);
    for (int i = 0; i < count; i++) {
        mqtt_publish(s_reset_topics[i], "", 1, true);
        ESP_LOGW(TAG, "HA entity reset: cleared %s", s_reset_topics[i]);
        free(s_reset_topics[i]);
        s_reset_topics[i] = NULL;
    }
    ESP_LOGW(TAG, "HA entity reset: cleared %d discovery topics, restarting", count);

    vTaskDelay(pdMS_TO_TICKS(HA_RESET_FLUSH_MS));
    esp_restart();
}

bool mqtt_discovery_reset_start(void)
{
    if (!mqtt_client_is_connected()) {
        ESP_LOGW(TAG, "HA entity reset needs an MQTT connection");
        return false;
    }

    // Claim the reset here rather than in the task: the web button and the
    // Home Assistant button can both fire before either task has started, and
    // two tasks walking the same topic list would double-free it.
    if (atomic_exchange(&s_reset_active, true)) {
        ESP_LOGW(TAG, "HA entity reset already running");
        return false;
    }
    // Safe to clear only after the claim, and before the task subscribes:
    // nothing can be appended until those retained configs start arriving.
    atomic_store(&s_reset_count, 0);

    if (xTaskCreate(ha_reset_task, "ha_reset", 4096, NULL, 5, NULL) != pdPASS) {
        atomic_store(&s_reset_active, false);
        ESP_LOGE(TAG, "HA entity reset: could not start task");
        return false;
    }
    return true;
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
    char entity_slug[48];
    if (single_sensor_source) {
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_temp_%s", mac_suffix, slug);
        snprintf(entity_slug, sizeof(entity_slug), "temp_%s", slug);
    } else {
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_temp_%s_%u",
                 mac_suffix, slug, sensor_index);
        snprintf(entity_slug, sizeof(entity_slug), "temp_%s_%u", slug, sensor_index);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", display_name);
    cJSON_AddStringToObject(root, "device_class", "temperature");
    cJSON_AddStringToObject(root, "icon", "mdi:thermometer");
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "unit_of_measurement", "°C");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.value }}");
    add_entity_ids(root, "sensor", mac_suffix, uid, "%s", entity_slug);
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
    add_entity_ids(root, "number", mac_suffix, uid, "heater_%d_%s_setpoint", index + 1, circuit);
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
    add_entity_ids(root, "switch", mac_suffix, uid, "heater_%d", index + 1);
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
        add_entity_ids(root, "sensor", mac_suffix, uid, "heater_%d_status", index + 1);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print discovery JSON for %s", display_name);
            cJSON_Delete(root);
            return;
        }
        publish_discovery("sensor", uid, json_str);
        cJSON_free(json_str);
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
        add_entity_ids(root, "binary_sensor", mac_suffix, uid, "heater_%d_water_flow", index + 1);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print discovery JSON for %s", display_name);
            cJSON_Delete(root);
            return;
        }
        publish_discovery("binary_sensor", uid, json_str);
        cJSON_free(json_str);
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
        add_entity_ids(root, "binary_sensor", mac_suffix, uid, "heater_%d_locked_out", index + 1);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print discovery JSON for %s", display_name);
            cJSON_Delete(root);
            return;
        }
        publish_discovery("binary_sensor", uid, json_str);
        cJSON_free(json_str);
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
        add_entity_ids(root, "sensor", mac_suffix, uid, "heater_%d_burner", index + 1);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print discovery JSON for %s", display_name);
            cJSON_Delete(root);
            return;
        }
        publish_discovery("sensor", uid, json_str);
        cJSON_free(json_str);
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
        add_entity_ids(root, "binary_sensor", mac_suffix, uid, "heater_%d_general_service", index + 1);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print discovery JSON for %s", display_name);
            cJSON_Delete(root);
            return;
        }
        publish_discovery("binary_sensor", uid, json_str);
        cJSON_free(json_str);
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
        add_entity_ids(root, "binary_sensor", mac_suffix, uid, "heater_%d_ignition_service", index + 1);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print discovery JSON for %s", display_name);
            cJSON_Delete(root);
            return;
        }
        publish_discovery("binary_sensor", uid, json_str);
        cJSON_free(json_str);
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
        add_entity_ids(root, "binary_sensor", mac_suffix, uid, "heater_%d_cooling_available", index + 1);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));
        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print discovery JSON for %s", display_name);
            cJSON_Delete(root);
            return;
        }
        publish_discovery("binary_sensor", uid, json_str);
        cJSON_free(json_str);
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

    add_entity_ids(root, "select", mac_suffix, uid, "mode");
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
                                      int channel_num, const char *channel_name,
                                      bool include_state_entities, bool include_power_sensors,
                                      bool include_pump_mode_select)
{
    char avail_topic[128];
    char state_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/channel/%d/state", device_id, channel_num);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/channel/%d/set", device_id, channel_num);

    // Display name only — unique IDs below are keyed on the stable channel
    // number alone, never on the name. The name can legitimately change
    // between discovery publishes (e.g. it's empty until the bus's channel-
    // name broadcast arrives, so an early publish falls back to the type
    // name); if that mutable value were embedded in the unique_id, each
    // change would register as a brand-new HA entity while the old one
    // lingers as an orphaned duplicate (retained on the broker forever).
    char display_name[64];
    if (channel_name && channel_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s", channel_name);
    } else {
        snprintf(display_name, sizeof(display_name), "Channel %d", channel_num);
    }

    // Build unique IDs
    char sensor_uid[64];
    char button_uid[64];
    char active_uid[64];
    snprintf(sensor_uid, sizeof(sensor_uid), DISCOVERY_ID_PREFIX "_%s_ch%d", mac_suffix, channel_num);
    snprintf(button_uid, sizeof(button_uid), DISCOVERY_ID_PREFIX "_%s_ch%d_toggle", mac_suffix, channel_num);
    snprintf(active_uid, sizeof(active_uid), DISCOVERY_ID_PREFIX "_%s_ch%d_active", mac_suffix, channel_num);

    if (include_state_entities) {
        // Sensor for state
        {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "name", display_name);
            cJSON_AddStringToObject(root, "state_topic", state_topic);
            cJSON_AddStringToObject(root, "value_template", "{{ value_json.state }}");
            add_entity_ids(root, "sensor", mac_suffix, sensor_uid, "channel_%d", channel_num);
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
            add_entity_ids(root, "button", mac_suffix, button_uid, "channel_%d_toggle", channel_num);
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
            add_entity_ids(root, "binary_sensor", mac_suffix, active_uid, "channel_%d_active", channel_num);
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

    // Number for the configured power estimate (Watts). 0 = unset, which
    // leaves the channel with nothing to report (see channel_power_get_effective).
    {
        char power_command_topic[128];
        snprintf(power_command_topic, sizeof(power_command_topic),
                 "pool/%s/channel/%d/power/set", device_id, channel_num);

        char power_uid[80];
        snprintf(power_uid, sizeof(power_uid), DISCOVERY_ID_PREFIX "_%s_ch%d_config_power", mac_suffix, channel_num);

        char power_name[96];
        snprintf(power_name, sizeof(power_name), "Power: %s", display_name);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", power_name);
        cJSON_AddStringToObject(root, "icon", "mdi:flash-outline");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "command_topic", power_command_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.configured_watts }}");
        cJSON_AddStringToObject(root, "unit_of_measurement", "W");
        cJSON_AddNumberToObject(root, "min", 0);
        cJSON_AddNumberToObject(root, "max", 10000);
        cJSON_AddNumberToObject(root, "step", 1);
        cJSON_AddStringToObject(root, "mode", "box");
        cJSON_AddStringToObject(root, "entity_category", "config");
        add_entity_ids(root, "number", mac_suffix, power_uid, "channel_%d_configured_power", channel_num);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print channel configured power discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("number", power_uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    }

    // Pump mode select — a direct state set via CMD 0x0F, as opposed to the
    // toggle button above, which can only advance the channel one step around
    // a fixed ring. Supplements that button rather than replacing it.
    //
    // The option list depends on whether this channel drives a multi-speed
    // pump, which the bus never states outright (see filter_pump_type.h). Until that
    // is known we offer Off/Auto/On: "On" is valid either way, and it is also
    // what resolves the question — a multi-speed channel answers it by
    // reporting a named speed, a single-speed one by reporting On. This entity is
    // deliberately not optimistic: the selection only sticks once the
    // Touchscreen's own 0x0B broadcast confirms it, so a command the
    // Touchscreen ignores visibly snaps back.
    char mode_uid[64];
    snprintf(mode_uid, sizeof(mode_uid), DISCOVERY_ID_PREFIX "_%s_ch%d_mode",
             mac_suffix, channel_num);

    if (include_pump_mode_select) {
        char mode_command_topic[128];
        snprintf(mode_command_topic, sizeof(mode_command_topic),
                 "pool/%s/channel/%d/mode/set", device_id, channel_num);

        char mode_name[80];
        snprintf(mode_name, sizeof(mode_name), "%s Mode", display_name);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", mode_name);
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "command_topic", mode_command_topic);

        // Option strings are CHANNEL_STATE_NAMES verbatim. The channel's state
        // topic publishes from the same table (mqtt_publish_channel) and the
        // command handler parses against it, so an option always matches the
        // state HA reads back.
        cJSON *opts = cJSON_CreateArray();
        cJSON_AddItemToArray(opts, cJSON_CreateString(CHANNEL_STATE_NAMES[0]));  // Off
        cJSON_AddItemToArray(opts, cJSON_CreateString(CHANNEL_STATE_NAMES[1]));  // Auto
        if (filter_pump_type_get() == FILTER_PUMP_TYPE_MULTI_SPEED) {
            // "On" is omitted deliberately: the Touchscreen normalises it to
            // High Speed on these channels and never reports it back, so it
            // would be an option that silently means something else.
            for (int i = 3; i < CHANNEL_STATE_COUNT; i++) {  // Low / Medium / High
                cJSON_AddItemToArray(opts, cJSON_CreateString(CHANNEL_STATE_NAMES[i]));
            }
        } else {
            cJSON_AddItemToArray(opts, cJSON_CreateString(CHANNEL_STATE_NAMES[2]));  // On
        }
        cJSON_AddItemToObject(root, "options", opts);

        cJSON_AddStringToObject(root, "value_template", "{{ value_json.state }}");
        add_entity_ids(root, "select", mac_suffix, mode_uid, "channel_%d_mode", channel_num);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print channel mode select discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("select", mode_uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    } else {
        // Retract it if this channel is no longer pump-driven, so a re-typed
        // channel doesn't leave an orphan select retained on the broker.
        remove_discovery("select", mode_uid);
    }

    // Power and energy sensors, only once the channel actually has a
    // configured wattage to work from. Without one they
    // could never report anything but unknown, so publishing them on a stock
    // install would just litter HA with dead entities. mqtt_publish_channel
    // re-runs discovery when the source appears or goes away, and the retract
    // below also cleans up entities left by earlier firmware that published
    // these unconditionally.
    if (!include_power_sensors) {
        char uid[80];
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_ch%d_power_sensor", mac_suffix, channel_num);
        remove_discovery("sensor", uid);
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_ch%d_energy", mac_suffix, channel_num);
        remove_discovery("sensor", uid);
        return;
    }

    // Sensor for live power: the configured estimate, gated by active state.
    {
        char power_sensor_uid[80];
        snprintf(power_sensor_uid, sizeof(power_sensor_uid), DISCOVERY_ID_PREFIX "_%s_ch%d_power_sensor", mac_suffix, channel_num);

        char power_sensor_name[80];
        snprintf(power_sensor_name, sizeof(power_sensor_name), "%s Power", display_name);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", power_sensor_name);
        cJSON_AddStringToObject(root, "device_class", "power");
        cJSON_AddStringToObject(root, "state_class", "measurement");
        cJSON_AddStringToObject(root, "unit_of_measurement", "W");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.power_watts }}");
        add_entity_ids(root, "sensor", mac_suffix, power_sensor_uid, "channel_%d_power", channel_num);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print channel power sensor discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("sensor", power_sensor_uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    }

    // Energy sensor: cumulative kWh, integrated on-device from the effective
    // power over time (see channel_energy.c). The accumulator lives in RAM
    // only and resets to 0 on reboot; state_class total_increasing tells HA
    // to treat a value drop as a meter reset rather than corrupt long-term
    // statistics, so this is safe without NVS persistence.
    {
        char energy_state_topic[128];
        snprintf(energy_state_topic, sizeof(energy_state_topic),
                 "pool/%s/channel/%d/energy/state", device_id, channel_num);

        char energy_uid[80];
        snprintf(energy_uid, sizeof(energy_uid), DISCOVERY_ID_PREFIX "_%s_ch%d_energy", mac_suffix, channel_num);

        char energy_name[96];
        snprintf(energy_name, sizeof(energy_name), "%s Energy", display_name);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", energy_name);
        cJSON_AddStringToObject(root, "device_class", "energy");
        cJSON_AddStringToObject(root, "state_class", "total_increasing");
        cJSON_AddStringToObject(root, "unit_of_measurement", "kWh");
        cJSON_AddStringToObject(root, "state_topic", energy_state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.energy_kwh }}");
        add_entity_ids(root, "sensor", mac_suffix, energy_uid, "channel_%d_energy", channel_num);
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print channel energy sensor discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("sensor", energy_uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    }
}

// ======================================================
// System Baseline Power Discovery
// ======================================================

// The always-on draw that isn't attributable to any one channel — the
// controller itself, chlorinator standby, and so on. Mirrors a channel's
// power trio (configurable wattage, live power, cumulative energy) but has
// no active state: a configured baseline is always drawing, so its power
// sensor simply reports the configured figure.
static void publish_system_power_discovery(const char *device_id, const char *mac_suffix,
                                           bool include_power_sensors)
{
    char avail_topic[128];
    char state_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(state_topic, sizeof(state_topic), "pool/%s/system/power/state", device_id);

    // Number for the configured baseline (Watts). Always published — it's how
    // the baseline gets set in the first place, so gating it would leave no
    // way to turn the sensors below on.
    {
        char command_topic[128];
        snprintf(command_topic, sizeof(command_topic), "pool/%s/system/power/set", device_id);

        char uid[80];
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_system_config_power", mac_suffix);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "Power: System");
        cJSON_AddStringToObject(root, "icon", "mdi:flash-outline");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "command_topic", command_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.configured_watts }}");
        cJSON_AddStringToObject(root, "unit_of_measurement", "W");
        cJSON_AddNumberToObject(root, "min", 0);
        cJSON_AddNumberToObject(root, "max", 10000);
        cJSON_AddNumberToObject(root, "step", 1);
        cJSON_AddStringToObject(root, "mode", "box");
        cJSON_AddStringToObject(root, "entity_category", "config");
        add_entity_ids(root, "number", mac_suffix, uid, "system_configured_power");
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print system configured power discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("number", uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    }

    // Power and energy sensors, only once a baseline is actually configured —
    // same reasoning as the per-channel pair (see publish_channel_discovery).
    if (!include_power_sensors) {
        char uid[80];
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_system_power_sensor", mac_suffix);
        remove_discovery("sensor", uid);
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_system_energy", mac_suffix);
        remove_discovery("sensor", uid);
        return;
    }

    {
        char uid[80];
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_system_power_sensor", mac_suffix);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "System Power");
        cJSON_AddStringToObject(root, "device_class", "power");
        cJSON_AddStringToObject(root, "state_class", "measurement");
        cJSON_AddStringToObject(root, "unit_of_measurement", "W");
        cJSON_AddStringToObject(root, "state_topic", state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.power_watts }}");
        add_entity_ids(root, "sensor", mac_suffix, uid, "system_power");
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print system power sensor discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("sensor", uid, json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
    }

    {
        char energy_state_topic[128];
        snprintf(energy_state_topic, sizeof(energy_state_topic),
                 "pool/%s/system/energy/state", device_id);

        char uid[80];
        snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_system_energy", mac_suffix);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "System Energy");
        cJSON_AddStringToObject(root, "device_class", "energy");
        cJSON_AddStringToObject(root, "state_class", "total_increasing");
        cJSON_AddStringToObject(root, "unit_of_measurement", "kWh");
        cJSON_AddStringToObject(root, "state_topic", energy_state_topic);
        cJSON_AddStringToObject(root, "value_template", "{{ value_json.energy_kwh }}");
        add_entity_ids(root, "sensor", mac_suffix, uid, "system_energy");
        cJSON_AddStringToObject(root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

        char *json_str = cJSON_PrintUnformatted(root);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to print system energy sensor discovery JSON");
            cJSON_Delete(root);
            return;
        }
        publish_discovery("sensor", uid, json_str);
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
    add_entity_ids(root, "light", mac_suffix, uid, "light_%d", zone_num);
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
        add_entity_ids(btn, "button", mac_suffix, resync_uid, "light_%d_resync", zone_num);
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
    // pH is dimensionless: HA's `ph` device class accepts no unit at all
    // (DEVICE_CLASS_UNITS maps it to {None}), so adding one costs the class.
    cJSON_AddStringToObject(root, "device_class", "ph");
    cJSON_AddStringToObject(root, "state_class", "measurement");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.ph }}");
    add_entity_ids(root, "sensor", mac_suffix, uid, "ph");
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
    add_entity_ids(root, "sensor", mac_suffix, uid, "orp");
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
    cJSON_AddStringToObject(root, "device_class", "ph");
    cJSON_AddStringToObject(root, "icon", "mdi:ph");
    add_entity_ids(root, "sensor", mac_suffix, uid, "ph_setpoint");
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
    add_entity_ids(root, "sensor", mac_suffix, uid, "orp_setpoint");
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
    add_entity_ids(root, "sensor", mac_suffix, uid, "chlor_output_level");
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
// Pump Sensor Discovery (speed, power, energy)
// ======================================================

static void publish_pump_discovery(const char *device_id, const char *mac_suffix,
                                   bool include_power_sensors)
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
    add_entity_ids(root, "sensor", mac_suffix, uid, "pump_speed");
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

    // Power and energy sensors, only once the pump has actually broadcast a
    // wattage — it also sends speed-only telemetry (CMD 0x3B with a 2-byte
    // payload), and a pump that only ever does that would leave both entities
    // sitting at unknown. Same reasoning as the per-channel pair (see
    // publish_channel_discovery), including the retraction that cleans them up
    // if the source never appears.
    char power_uid[64];
    char energy_uid[64];
    snprintf(power_uid, sizeof(power_uid), DISCOVERY_ID_PREFIX "_%s_pump_power", mac_suffix);
    snprintf(energy_uid, sizeof(energy_uid), DISCOVERY_ID_PREFIX "_%s_pump_energy", mac_suffix);

    if (!include_power_sensors) {
        remove_discovery("sensor", power_uid);
        remove_discovery("sensor", energy_uid);
        return;
    }

    // The pump's own metered draw, reported independently of the Filter
    // channel that switches it. The channel's configured estimate covers
    // whatever else is wired to that channel (a chlorinator, say) and never
    // includes these watts, so the two don't overlap.
    {
        cJSON *power_root = cJSON_CreateObject();
        cJSON_AddStringToObject(power_root, "name", "Pump Power");
        cJSON_AddStringToObject(power_root, "device_class", "power");
        cJSON_AddStringToObject(power_root, "state_class", "measurement");
        cJSON_AddStringToObject(power_root, "state_topic", state_topic);
        cJSON_AddStringToObject(power_root, "value_template", "{{ value_json.power_watts }}");
        cJSON_AddStringToObject(power_root, "unit_of_measurement", "W");
        cJSON_AddStringToObject(power_root, "icon", "mdi:flash");
        add_entity_ids(power_root, "sensor", mac_suffix, power_uid, "pump_power");
        cJSON_AddStringToObject(power_root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(power_root, "device", build_device_cjson(device_id, mac_suffix));

        char *power_json_str = cJSON_PrintUnformatted(power_root);
        if (!power_json_str) {
            ESP_LOGE(TAG, "Failed to print pump power discovery JSON");
            cJSON_Delete(power_root);
            return;
        }
        publish_discovery("sensor", power_uid, power_json_str);
        cJSON_free(power_json_str);
        cJSON_Delete(power_root);
    }

    // Cumulative kWh, integrated on-device from the pump's reported watts
    // (see channel_energy.c) — the one energy figure on this device that
    // comes from real metering rather than an estimate. RAM-only, so
    // total_increasing lets HA treat the reboot reset as a meter reset.
    {
        char energy_state_topic[128];
        snprintf(energy_state_topic, sizeof(energy_state_topic), "pool/%s/pump/energy/state", device_id);

        cJSON *energy_root = cJSON_CreateObject();
        cJSON_AddStringToObject(energy_root, "name", "Pump Energy");
        cJSON_AddStringToObject(energy_root, "device_class", "energy");
        cJSON_AddStringToObject(energy_root, "state_class", "total_increasing");
        cJSON_AddStringToObject(energy_root, "unit_of_measurement", "kWh");
        cJSON_AddStringToObject(energy_root, "state_topic", energy_state_topic);
        cJSON_AddStringToObject(energy_root, "value_template", "{{ value_json.energy_kwh }}");
        add_entity_ids(energy_root, "sensor", mac_suffix, energy_uid, "pump_energy");
        cJSON_AddStringToObject(energy_root, "availability_topic", avail_topic);
        cJSON_AddItemToObject(energy_root, "device", build_device_cjson(device_id, mac_suffix));

        char *energy_json_str = cJSON_PrintUnformatted(energy_root);
        if (!energy_json_str) {
            ESP_LOGE(TAG, "Failed to print pump energy discovery JSON");
            cJSON_Delete(energy_root);
            return;
        }
        publish_discovery("sensor", energy_uid, energy_json_str);
        cJSON_free(energy_json_str);
        cJSON_Delete(energy_root);
    }
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
    add_entity_ids(root, "binary_sensor", mac_suffix, uid, "service_mode");
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
void mqtt_publish_service_mode_discovery_single(void)      { publish_single(publish_service_mode_discovery); }

void mqtt_publish_pump_discovery_single(bool include_power_sensors)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    publish_pump_discovery(device_id, mac_suffix, include_power_sensors);
}

void mqtt_publish_channel_discovery_single(int channel_num, const char *channel_name,
                                           bool include_state_entities, bool include_power_sensors,
                                           bool include_pump_mode_select)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    ESP_LOGI(TAG, "Publishing discovery for channel %d: %s", channel_num, channel_name);
    publish_channel_discovery(device_id, mac_suffix, channel_num, channel_name,
                              include_state_entities, include_power_sensors,
                              include_pump_mode_select);
}

void mqtt_publish_system_power_discovery_single(bool include_power_sensors)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char mac_suffix[DEVICE_MAC_SUFFIX_LEN];
    device_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    publish_system_power_discovery(device_id, mac_suffix, include_power_sensors);
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
    add_entity_ids(root, "select", mac_suffix, uid, "valve_%d", valve_num);
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
    add_entity_ids(root, "select", mac_suffix, uid, "favourite");
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
    cJSON_AddStringToObject(root, "name", "System: Update Firmware");
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
    add_entity_ids(root, "update", mac_suffix, uid, "firmware");
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
    cJSON_AddStringToObject(root, "name", "System: Check Updates");
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "icon", "mdi:update");
    // Same section as the Firmware update entity it belongs with.
    cJSON_AddStringToObject(root, "entity_category", "config");
    add_entity_ids(root, "button", mac_suffix, uid, "firmware_check");
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
    cJSON_AddStringToObject(root, "name", "System: Reboot");
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "device_class", "restart");
    cJSON_AddStringToObject(root, "entity_category", "config");
    add_entity_ids(root, "button", mac_suffix, uid, "reboot");
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

// Button that clears every retained discovery config for this device and
// reboots, so Home Assistant re-creates the entities under the current
// entity_id scheme. Mirrors the web UI's reset button on the firmware-update
// page. The button deletes itself along with the rest and comes back after the
// reboot.
static void publish_ha_reset_discovery(const char *device_id, const char *mac_suffix)
{
    char avail_topic[128];
    char command_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "pool/%s/availability", device_id);
    snprintf(command_topic, sizeof(command_topic), "pool/%s/ha_reset", device_id);

    char uid[80];
    snprintf(uid, sizeof(uid), DISCOVERY_ID_PREFIX "_%s_ha_reset", mac_suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "System: Reset HA Entities");
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "icon", "mdi:tag-multiple");
    cJSON_AddStringToObject(root, "entity_category", "config");
    add_entity_ids(root, "button", mac_suffix, uid, "ha_reset");
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddItemToObject(root, "device", build_device_cjson(device_id, mac_suffix));

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print HA reset button discovery JSON");
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

    // Reboot and HA-entity-reset buttons
    publish_reboot_discovery(device_id, mac_suffix);
    publish_ha_reset_discovery(device_id, mac_suffix);

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
