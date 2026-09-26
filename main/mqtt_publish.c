#include "mqtt_publish.h"
#include "mqtt_poolclient.h"
#include "mqtt_discovery.h"
#include "pool_state.h"
#include "message_decoder.h"
#include "channel_power.h"
#include "filter_pump_type.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "MQTT_PUBLISH";

// Last published state (for change detection) - using pool_state_t as single source of truth
static pool_state_t s_last_published_state = {0};

// Track whether discovery has been published for each channel/light/valve/heater/favourite/temp-sensor
static struct {
    bool channels[MAX_CHANNELS];
    bool lights[MAX_LIGHT_ZONES];
    bool valves[MAX_VALVE_SLOTS];
    bool heaters[MAX_HEATERS];
    bool gas_heaters[MAX_HEATERS];
    bool heater_setpoints[MAX_HEATERS];
    bool favourite;
    bool temp_sensors[MAX_SEEN_DEVICES][2];   // [dev_idx][sensor_index-1]
    bool ph;
    bool orp;
    bool ph_setpoint;
    bool orp_setpoint;
    bool chlor_output_level;
    bool pump;
    bool service_mode;
} s_discovery_published = {0};

// Effective multicolor light type each zone's discovery was last published with
// (drives the color effect list); only meaningful while the zone's
// s_discovery_published.lights flag is set
static uint8_t s_light_discovery_type[MAX_LIGHT_ZONES];

// Last-published per-channel power fields. These aren't bus-observed state
// (channel_state_t), so they're tracked separately: a channel's power can
// change (via MQTT command) without any bus-driven channel state change.
static bool s_power_fields_published[MAX_CHANNELS];
static uint16_t s_last_configured_watts[MAX_CHANNELS];
static bool s_last_power_watts_valid[MAX_CHANNELS];

// Pump type the Filter channel's pump-mode select was last published with, so
// a change can force a re-publish with the right option list.
static filter_pump_type_t s_last_filter_pump_type = FILTER_PUMP_TYPE_UNKNOWN;

// Whether each channel's discovery was last published with the pump-mode
// select, so a re-typed channel re-runs discovery and the select is added or
// retracted even when its display name stays the same.
static bool s_channel_discovery_pump_select[MAX_CHANNELS];
static uint16_t s_last_power_watts[MAX_CHANNELS];

// Whether the pump's discovery was last published with its power and energy
// sensors. They only exist once the pump has broadcast a wattage — it also
// sends speed-only telemetry — so a flip has to re-run discovery.
static bool s_pump_discovery_with_power;

// Display name each channel's discovery was last published with. Unique IDs
// are stable (keyed on channel number only — see publish_channel_discovery),
// so it's safe to re-publish discovery whenever the name changes (e.g. the
// bus's channel-name broadcast arrives after the type-name fallback was
// already used); HA updates the entity's label in place rather than forking
// a new one.
static char s_channel_discovery_name[MAX_CHANNELS][32];

// ======================================================
// Setpoint and Temperature Publishing
// ======================================================

void mqtt_publish_heater_setpoints(const pool_state_t *current_state, int index)
{
    if (index < 0 || index >= MAX_HEATERS) {
        return;
    }

    const pool_heater_t *heater = &current_state->heaters[index];
    const pool_heater_t *last = &s_last_published_state.heaters[index];

    // Check if anything changed
    if (last->pool_setpoint == heater->pool_setpoint &&
        last->spa_setpoint == heater->spa_setpoint &&
        s_last_published_state.temp_scale_fahrenheit == current_state->temp_scale_fahrenheit) {
        return;  // No change, skip publish
    }

    // Publish discovery on first publish
    if (!s_discovery_published.heater_setpoints[index]) {
        mqtt_publish_heater_setpoint_discovery_single(index);
        s_discovery_published.heater_setpoints[index] = true;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/heater/%d/setpoints/state", device_id, index);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"pool_sp\":%d,\"spa_sp\":%d,\"scale\":\"%s\"}",
             heater->pool_setpoint, heater->spa_setpoint,
             current_state->temp_scale_fahrenheit ? "F" : "C");

    mqtt_publish(topic, payload, 0, true);

    s_last_published_state.heaters[index].pool_setpoint = heater->pool_setpoint;
    s_last_published_state.heaters[index].spa_setpoint = heater->spa_setpoint;
    s_last_published_state.temp_scale_fahrenheit = current_state->temp_scale_fahrenheit;

    ESP_LOGI(TAG, "Published heater %d setpoints: pool=%d, spa=%d, scale=%s",
             index, heater->pool_setpoint, heater->spa_setpoint,
             current_state->temp_scale_fahrenheit ? "F" : "C");
}

void mqtt_publish_temperature_reading(const pool_state_t *current_state, int dev_idx, uint8_t sensor_index)
{
    if (dev_idx < 0 || dev_idx >= current_state->num_seen_devices) return;
    if (sensor_index < 1 || sensor_index > 2) return;

    const seen_device_t *d = &current_state->seen_devices[dev_idx];
    uint8_t value = (sensor_index == 1) ? d->temp1 : d->temp2;
    bool    valid = (sensor_index == 1) ? d->temp1_valid : d->temp2_valid;
    if (!valid) return;

    // Change detection against the per-device cached snapshot.
    if (dev_idx < s_last_published_state.num_seen_devices) {
        const seen_device_t *last = &s_last_published_state.seen_devices[dev_idx];
        if (sensor_index == 1 && last->temp1_valid && last->temp1 == value) return;
        if (sensor_index == 2 && last->temp2_valid && last->temp2 == value) return;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char slug[24];
    get_device_slug(d->addr_hi, d->addr_lo, slug, sizeof(slug));

    // Lazy HA discovery — published once per (dev_idx, sensor_index) on first reading.
    if (dev_idx < MAX_SEEN_DEVICES && !s_discovery_published.temp_sensors[dev_idx][sensor_index - 1]) {
        mqtt_publish_temperature_sensor_discovery_single(
            d->addr_hi, d->addr_lo, sensor_index, d->single_sensor_source);
        s_discovery_published.temp_sensors[dev_idx][sensor_index - 1] = true;
    }

    char topic[160];
    if (d->single_sensor_source) {
        snprintf(topic, sizeof(topic), "pool/%s/temperature/%s/state", device_id, slug);
    } else {
        snprintf(topic, sizeof(topic), "pool/%s/temperature/%s/%u/state", device_id, slug, sensor_index);
    }

    char payload[64];
    snprintf(payload, sizeof(payload), "{\"value\":%d}", value);
    mqtt_publish(topic, payload, 0, true);

    // Mirror the published device entry into the cache.
    if (dev_idx < MAX_SEEN_DEVICES) {
        s_last_published_state.seen_devices[dev_idx] = *d;
        if (dev_idx >= s_last_published_state.num_seen_devices) {
            s_last_published_state.num_seen_devices = dev_idx + 1;
        }
    }

    if (d->single_sensor_source) {
        ESP_LOGI(TAG, "Published temperature: %s=%d°C", slug, value);
    } else {
        ESP_LOGI(TAG, "Published temperature: %s/%u=%d°C", slug, sensor_index, value);
    }
}

// ======================================================
// Heater Publishing
// ======================================================

void mqtt_publish_heater(const pool_state_t *current_state, int index)
{
    if (index < 0 || index >= MAX_HEATERS) {
        return;
    }

    const pool_heater_t *heater = &current_state->heaters[index];

    // Check if anything changed
    if (s_last_published_state.heaters[index].valid &&
        s_last_published_state.heaters[index].on == heater->on) {
        return;  // No change, skip publish
    }

    // Publish discovery on first publish
    if (!s_discovery_published.heaters[index]) {
        mqtt_publish_heater_discovery_single(index);
        s_discovery_published.heaters[index] = true;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/heater/%d/state", device_id, index);

    const char *payload = heater->on ? "ON" : "OFF";
    mqtt_publish(topic, payload, 0, true);

    // Update last published state
    s_last_published_state.heaters[index].on = heater->on;
    s_last_published_state.heaters[index].valid = true;

    ESP_LOGI(TAG, "Published heater %d: %s", index, payload);
}

void mqtt_publish_gas_heater(const pool_state_t *current_state, int index)
{
    if (index < 0 || index >= MAX_HEATERS) {
        ESP_LOGE(TAG, "mqtt_publish_gas_heater: index %d out of range", index);
        return;
    }

    const pool_heater_t *heater = &current_state->heaters[index];

    if (!heater->gas_heater_valid) {
        ESP_LOGE(TAG, "mqtt_publish_gas_heater: called with gas_heater_valid=false for index %d", index);
        return;
    }

    // status is the single source of truth — all derived fields follow from it
    const pool_heater_t *last = &s_last_published_state.heaters[index];
    if (last->gas_heater_valid &&
        last->status == heater->status &&
        last->general_service_required == heater->general_service_required &&
        last->ignition_service_required == heater->ignition_service_required &&
        last->cooling_available == heater->cooling_available) {
        return;
    }

    if (!s_discovery_published.gas_heaters[index]) {
        mqtt_publish_gas_heater_discovery_single(index);
        s_discovery_published.gas_heaters[index] = true;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/heater/%d/gas_status/state", device_id, index);

    const char *status_name = (heater->status < HEATER_STATUS_NAME_COUNT)
                               ? HEATER_STATUS_NAMES[heater->status] : "unknown";
    const char *burner_name = (heater->burner_state < BURNER_STATE_NAME_COUNT)
                               ? BURNER_STATE_NAMES[heater->burner_state] : "unknown";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"%s\",\"water_flow\":%s,\"locked_out\":%s,\"burner\":\"%s\","
             "\"general_service_required\":%s,\"ignition_service_required\":%s,\"cooling_available\":%s}",
             status_name,
             heater->water_flow_detected ? "true" : "false",
             heater->locked_out ? "true" : "false",
             burner_name,
             heater->general_service_required ? "true" : "false",
             heater->ignition_service_required ? "true" : "false",
             heater->cooling_available ? "true" : "false");

    mqtt_publish(topic, payload, 0, true);

    s_last_published_state.heaters[index].gas_heater_valid = true;
    s_last_published_state.heaters[index].status = heater->status;
    s_last_published_state.heaters[index].general_service_required = heater->general_service_required;
    s_last_published_state.heaters[index].ignition_service_required = heater->ignition_service_required;
    s_last_published_state.heaters[index].cooling_available = heater->cooling_available;

    ESP_LOGI(TAG, "Published gas heater %d: %s", index, payload);
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

    const char *payload = (current_state->mode == MODE_SPA) ? "Spa" : (current_state->mode == MODE_POOL) ? "Pool" : "Unknown";
    mqtt_publish(topic, payload, 0, true);

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
    if (channel_id < 1 || channel_id > MAX_CHANNELS) {
        return;
    }

    int idx = channel_id - 1;
    const channel_state_t *channel = &current_state->channels[idx];

    // Skip unconfigured/unused channels
    if (!channel->configured) {
        ESP_LOGI(TAG, "Skipping unconfigured channel %d", channel_id);
        return;
    }

    // Heater and light zone channel slots get their own state/toggle/active
    // entities from mqtt_publish_heater*/mqtt_publish_light — publishing them
    // again here would be redundant (and the toggle command isn't the right
    // way to control them anyway). They still get power/energy tracking below.
    bool include_state_entities = (channel->type != CHANNEL_TYPE_HEATER &&
                                    channel->type != CHANNEL_TYPE_LIGHT_ZONE);

    // Use channel name if set, otherwise fall back to type name. Light zone
    // channels are the exception: the controller doesn't populate the raw
    // channel name (0x7C+) for them, so the zone's real assigned name (e.g.
    // "Pool", "Spa") is always preferred instead, from the separate
    // lighting[] array (register 0xB0+). Light zones are numbered by
    // configuration order among light-typed channels, not by physical
    // channel slot — e.g. if channel 3 is the first (lowest-numbered)
    // channel typed as a light zone, it's Light Zone 1 regardless of its
    // channel number — so find this channel's rank among light-typed
    // channels to get its lighting[] index.
    const char *type_name = get_channel_type_name(channel->type);
    const char *display_name = type_name;
    char heater_name_buf[16];  // only used in the CHANNEL_TYPE_HEATER branch below
    if (channel->type == CHANNEL_TYPE_LIGHT_ZONE) {
        int zone_rank = 0;
        for (int i = 0; i < channel_id; i++) {
            if (current_state->channels[i].configured &&
                current_state->channels[i].type == CHANNEL_TYPE_LIGHT_ZONE) {
                zone_rank++;
            }
        }
        int zone_idx = zone_rank - 1;
        if (zone_idx >= 0 && zone_idx < MAX_LIGHT_ZONES) {
            const lighting_state_t *light = &current_state->lighting[zone_idx];
            if (light->name_valid && light->name_id < LIGHT_ZONE_NAME_COUNT) {
                display_name = LIGHT_ZONE_NAME_TABLE[light->name_id];
            }
        }
    } else if (channel->type == CHANNEL_TYPE_HEATER) {
        // Heaters have no per-unit custom name (pool_heater_t carries no
        // name field) — mqtt_publish_heater/publish_heater_discovery always
        // refer to them generically as "Heater N" by index. Match that here:
        // rank this channel among heater-typed channels (same configuration-
        // order reasoning as light zones above) to get its heater number.
        int heater_rank = 0;
        for (int i = 0; i < channel_id; i++) {
            if (current_state->channels[i].configured &&
                current_state->channels[i].type == CHANNEL_TYPE_HEATER) {
                heater_rank++;
            }
        }
        if (heater_rank >= 1 && heater_rank <= MAX_HEATERS) {
            snprintf(heater_name_buf, sizeof(heater_name_buf), "Heater %d", heater_rank);
            display_name = heater_name_buf;
        }
    } else if (channel->name[0] != '\0') {
        display_name = channel->name;
    }

    uint16_t configured_watts = channel_power_get_configured(channel_id);
    uint16_t power_watts = 0;
    bool power_watts_valid = channel_power_get_effective(current_state, channel_id, &power_watts);

    // Re-publish discovery if the display name changed since it was last
    // sent (e.g. the bus's channel-name broadcast arrived after an earlier
    // publish already used the type-name fallback). Unique IDs are stable
    // (channel number only), so this updates the entity's label in HA
    // in place rather than creating a duplicate. Also re-publish when the
    // channel gains or loses a configured wattage, since that decides whether
    // the power and energy sensors exist at all.
    // The pump-mode select's option list depends on the learned pump type, so
    // a change there (typically once, the first time the pump runs) has to
    // reach HA as a fresh discovery payload, and so does the channel gaining or
    // losing the select altogether when it is re-typed.
    bool pump_mode_select = (channel->type == CHANNEL_TYPE_FILTER);
    filter_pump_type_t filter_pump_type = filter_pump_type_get();

    if (s_discovery_published.channels[idx] &&
        (strcmp(s_channel_discovery_name[idx], display_name) != 0 ||
         s_last_power_watts_valid[idx] != power_watts_valid ||
         s_channel_discovery_pump_select[idx] != pump_mode_select ||
         (pump_mode_select && s_last_filter_pump_type != filter_pump_type))) {
        s_discovery_published.channels[idx] = false;
    }

    // Publish discovery if this is the first time seeing this channel (or the name changed)
    if (!s_discovery_published.channels[idx]) {
        mqtt_publish_channel_discovery_single(channel_id, display_name, include_state_entities,
                                              power_watts_valid, pump_mode_select);
        if (pump_mode_select) {
            s_last_filter_pump_type = filter_pump_type;
        }
        s_channel_discovery_pump_select[idx] = pump_mode_select;
        s_discovery_published.channels[idx] = true;
        strncpy(s_channel_discovery_name[idx], display_name, sizeof(s_channel_discovery_name[idx]) - 1);
        s_channel_discovery_name[idx][sizeof(s_channel_discovery_name[idx]) - 1] = '\0';
    }

    // Check if anything changed (bus-observed state, or the power config)
    bool state_changed =
        !s_last_published_state.channels[idx].configured ||
        s_last_published_state.channels[idx].type != channel->type ||
        s_last_published_state.channels[idx].state != channel->state ||
        s_last_published_state.channels[idx].active != channel->active ||
        strcmp(s_last_published_state.channels[idx].name, channel->name) != 0;

    bool power_changed =
        !s_power_fields_published[idx] ||
        s_last_configured_watts[idx] != configured_watts ||
        s_last_power_watts_valid[idx] != power_watts_valid ||
        (power_watts_valid && s_last_power_watts[idx] != power_watts);

    if (!state_changed && !power_changed) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/channel/%d/state", device_id, channel_id);

    // State names
    const char *state_name = (channel->state < CHANNEL_STATE_COUNT)
                             ? CHANNEL_STATE_NAMES[channel->state] : "Unknown";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state",  state_name);
    cJSON_AddBoolToObject(root,   "active", channel->active);
    cJSON_AddStringToObject(root, "name",   display_name);
    cJSON_AddNumberToObject(root, "configured_watts", configured_watts);
    if (power_watts_valid) {
        cJSON_AddNumberToObject(root, "power_watts", power_watts);
    } else {
        cJSON_AddNullToObject(root, "power_watts");
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(topic, payload, 0, true);
        free(payload);
    }

    // Update last published state
    s_last_published_state.channels[idx].type = channel->type;
    s_last_published_state.channels[idx].state = channel->state;
    s_last_published_state.channels[idx].active = channel->active;
    strncpy(s_last_published_state.channels[idx].name, channel->name, sizeof(s_last_published_state.channels[idx].name) - 1);
    s_last_published_state.channels[idx].configured = true;

    s_power_fields_published[idx] = true;
    s_last_configured_watts[idx] = configured_watts;
    s_last_power_watts_valid[idx] = power_watts_valid;
    s_last_power_watts[idx] = power_watts;

    ESP_LOGI(TAG, "Published channel %d: %s (%s)", channel_id, state_name, display_name);
}

// Publish the system baseline's configured wattage and live power, along with
// the discovery that describes them. Both are cheap and this runs only on MQTT
// connect and when the user changes the value, so it republishes
// unconditionally rather than tracking what was last sent — which also means a
// publish attempted while MQTT is down can't leave discovery permanently
// unsent, since the next connect repeats it.
void mqtt_publish_system_power(void)
{
    uint16_t watts = channel_power_get_system();

    // A configured baseline is by definition always drawing, so live power is
    // just the configured figure; 0 means unset, and the sensors are dropped.
    mqtt_publish_system_power_discovery_single(watts != 0);

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/system/power/state", device_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "configured_watts", watts);
    cJSON_AddNumberToObject(root, "power_watts", watts);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(topic, payload, 0, true);
        free(payload);
    }
}

void mqtt_publish_system_energy(double energy_kwh)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/system/energy/state", device_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "energy_kwh", energy_kwh);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(topic, payload, 0, true);
        free(payload);
    }
}

void mqtt_publish_channel_energy(uint8_t channel_id, double energy_kwh)
{
    if (channel_id < 1 || channel_id > MAX_CHANNELS) {
        return;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/channel/%d/energy/state", device_id, channel_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "energy_kwh", energy_kwh);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(topic, payload, 0, true);
        free(payload);
    }
}

// ======================================================
// Lighting Publishing
// ======================================================

void mqtt_publish_light(const pool_state_t *current_state, uint8_t zone)
{
    if (zone < 1 || zone > MAX_LIGHT_ZONES) {
        return;
    }

    int idx = zone - 1;
    const lighting_state_t *light = &current_state->lighting[idx];

    // Skip unconfigured/unused lighting zones
    if (!light->configured) {
        ESP_LOGI(TAG, "Skipping unconfigured light zone %d", zone);
        return;
    }

    const char *zone_name = (light->name_valid && light->name_id < LIGHT_ZONE_NAME_COUNT) ?
                            LIGHT_ZONE_NAME_TABLE[light->name_id] : NULL;

    // Multicolor light type in effect for this zone: the system-wide model
    // selection, but only once the zone is known multicolor-capable
    uint8_t effective_light_type =
        (light->multicolor_valid && light->multicolor && current_state->multicolor_light_type_valid) ?
        current_state->multicolor_light_type : MULTICOLOR_LIGHT_TYPE_NONE;

    // Re-publish discovery if the zone name or the effective light type
    // (drives the color effect list) changed since last publish
    if (s_discovery_published.lights[idx] &&
        (s_last_published_state.lighting[idx].name_id != light->name_id ||
         s_last_published_state.lighting[idx].name_valid != light->name_valid ||
         s_light_discovery_type[idx] != effective_light_type)) {
        s_discovery_published.lights[idx] = false;
    }

    // Publish discovery if this is the first time seeing this light zone (or config changed)
    if (!s_discovery_published.lights[idx]) {
        mqtt_publish_light_discovery_single(zone, zone_name, effective_light_type);
        s_discovery_published.lights[idx] = true;
        s_light_discovery_type[idx] = effective_light_type;
    }

    // Check if anything changed
    if (s_last_published_state.lighting[idx].configured &&
        s_last_published_state.lighting[idx].state == light->state &&
        s_last_published_state.lighting[idx].color == light->color &&
        s_last_published_state.lighting[idx].active == light->active &&
        s_last_published_state.lighting[idx].multicolor == light->multicolor &&
        s_last_published_state.lighting[idx].multicolor_valid == light->multicolor_valid &&
        s_last_published_state.lighting[idx].name_id == light->name_id &&
        s_last_published_state.lighting[idx].name_valid == light->name_valid) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/light/%d/state", device_id, zone);

    // State names
    static const char *STATE_NAMES[] = {"Off", "Auto", "On"};
    const char *state_name = (light->state < 3) ? STATE_NAMES[light->state] : "Unknown";

    // Color names from the shared color table — must match the discovery
    // effect_list entries so HA can bind the current effect
    const char *color_name = (light->color < LIGHTING_COLOR_COUNT) ?
                             LIGHTING_COLOR_NAMES[light->color] : "Unknown";

    char fallback_name[24];
    if (!zone_name) {
        snprintf(fallback_name, sizeof(fallback_name), "Light Zone %d", zone);
    }
    const char *display_name = zone_name ? zone_name : fallback_name;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_name);
    cJSON_AddStringToObject(root, "color", color_name);
    cJSON_AddBoolToObject(root,   "active", light->active);
    cJSON_AddStringToObject(root, "name",  display_name);
    if (light->multicolor_valid) {
        cJSON_AddBoolToObject(root, "multicolor", light->multicolor);
    } else {
        cJSON_AddNullToObject(root, "multicolor");
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(topic, payload, 0, true);
        free(payload);
    }

    // Update last published state
    s_last_published_state.lighting[idx].state = light->state;
    s_last_published_state.lighting[idx].color = light->color;
    s_last_published_state.lighting[idx].active = light->active;
    s_last_published_state.lighting[idx].multicolor = light->multicolor;
    s_last_published_state.lighting[idx].multicolor_valid = light->multicolor_valid;
    s_last_published_state.lighting[idx].name_id = light->name_id;
    s_last_published_state.lighting[idx].name_valid = light->name_valid;
    s_last_published_state.lighting[idx].configured = true;

    ESP_LOGI(TAG, "Published light %d: %s, %s, active=%d, name=%s, multicolor=%d",
             zone, state_name, color_name, light->active, display_name, light->multicolor);
}

// ======================================================
// Chlorinator Publishing
// ======================================================

void mqtt_publish_chlorinator(const pool_state_t *current_state)
{
    ESP_LOGI(TAG, "Prepare to Publish chlorinator: pH=%d (valid=%d), ORP=%d (valid=%d), pH_sp=%d, ORP_sp=%d, salt=%d (valid=%d)",
             current_state->ph_reading, current_state->ph_valid,
             current_state->orp_reading, current_state->orp_valid,
             current_state->ph_setpoint, current_state->orp_setpoint,
             current_state->chlor_output_level, current_state->chlor_output_level_valid);

    // Check if anything changed
    if (s_last_published_state.ph_valid == current_state->ph_valid &&
        s_last_published_state.orp_valid == current_state->orp_valid &&
        s_last_published_state.ph_setpoint_valid == current_state->ph_setpoint_valid &&
        s_last_published_state.orp_setpoint_valid == current_state->orp_setpoint_valid &&
        s_last_published_state.ph_reading == current_state->ph_reading &&
        s_last_published_state.orp_reading == current_state->orp_reading &&
        s_last_published_state.ph_setpoint == current_state->ph_setpoint &&
        s_last_published_state.orp_setpoint == current_state->orp_setpoint &&
        s_last_published_state.chlor_output_level_valid == current_state->chlor_output_level_valid &&
        s_last_published_state.chlor_output_level == current_state->chlor_output_level) {
        return;  // No change, skip publish
    }

    // Publish discovery lazily, per entity, on its first valid value
    if (current_state->ph_valid && !s_discovery_published.ph) {
        mqtt_publish_ph_discovery_single();
        s_discovery_published.ph = true;
    }
    if (current_state->orp_valid && !s_discovery_published.orp) {
        mqtt_publish_orp_discovery_single();
        s_discovery_published.orp = true;
    }
    if (current_state->ph_setpoint_valid && !s_discovery_published.ph_setpoint) {
        mqtt_publish_ph_setpoint_discovery_single();
        s_discovery_published.ph_setpoint = true;
    }
    if (current_state->orp_setpoint_valid && !s_discovery_published.orp_setpoint) {
        mqtt_publish_orp_setpoint_discovery_single();
        s_discovery_published.orp_setpoint = true;
    }
    if (current_state->chlor_output_level_valid && !s_discovery_published.chlor_output_level) {
        mqtt_publish_chlor_output_level_discovery_single();
        s_discovery_published.chlor_output_level = true;
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
    if (current_state->ph_setpoint_valid) {
        len += snprintf(payload + len, sizeof(payload) - len,
                       ",\"ph_setpoint\":%.1f", current_state->ph_setpoint / 10.0);
    } else {
        len += snprintf(payload + len, sizeof(payload) - len, ",\"ph_setpoint\":null");
    }

    // ORP reading
    if (current_state->orp_valid) {
        len += snprintf(payload + len, sizeof(payload) - len,
                       ",\"orp\":%d", current_state->orp_reading);
    } else {
        len += snprintf(payload + len, sizeof(payload) - len, ",\"orp\":null");
    }

    // ORP setpoint
    if (current_state->orp_setpoint_valid) {
        len += snprintf(payload + len, sizeof(payload) - len,
                       ",\"orp_setpoint\":%d", current_state->orp_setpoint);
    } else {
        len += snprintf(payload + len, sizeof(payload) - len, ",\"orp_setpoint\":null");
    }

    // Salt chlorinator setpoint
    if (current_state->chlor_output_level_valid) {
        len += snprintf(payload + len, sizeof(payload) - len,
                       ",\"chlor_output_level\":%d}", current_state->chlor_output_level);
    } else {
        len += snprintf(payload + len, sizeof(payload) - len, "}");
    }

    mqtt_publish(topic, payload, 0, true);

    // Update last published state
    s_last_published_state.ph_reading = current_state->ph_reading;
    s_last_published_state.orp_reading = current_state->orp_reading;
    s_last_published_state.ph_setpoint = current_state->ph_setpoint;
    s_last_published_state.orp_setpoint = current_state->orp_setpoint;
    s_last_published_state.ph_valid = current_state->ph_valid;
    s_last_published_state.orp_valid = current_state->orp_valid;
    s_last_published_state.ph_setpoint_valid = current_state->ph_setpoint_valid;
    s_last_published_state.orp_setpoint_valid = current_state->orp_setpoint_valid;
    s_last_published_state.chlor_output_level = current_state->chlor_output_level;
    s_last_published_state.chlor_output_level_valid = current_state->chlor_output_level_valid;

    ESP_LOGI(TAG, "Published chlorinator: pH=%.1f (sp=%.1f), ORP=%d (sp=%d), chlor_output_level=%d",
             current_state->ph_reading / 10.0, current_state->ph_setpoint / 10.0,
             current_state->orp_reading, current_state->orp_setpoint,
             current_state->chlor_output_level);
}

// ======================================================
// Pump Publishing
// ======================================================

void mqtt_publish_pump(const pool_state_t *current_state)
{
    ESP_LOGD(TAG, "mqtt_publish_pump: speed_rpm=%d valid=%d",
             current_state->pump_speed, current_state->pump_speed_valid);

    // Check if anything changed
    if (s_last_published_state.pump_speed_valid == current_state->pump_speed_valid &&
        s_last_published_state.pump_speed == current_state->pump_speed &&
        s_last_published_state.pump_power_watts_valid == current_state->pump_power_watts_valid &&
        s_last_published_state.pump_power_watts == current_state->pump_power_watts) {
        return;  // No change, skip publish
    }

    // Publish discovery on first valid reading, and again if the pump's power
    // and energy sensors gain a source: a pump that has only sent speed-only
    // telemetry so far has no wattage to report, so those two are retracted
    // until the first 4-byte broadcast arrives.
    bool with_power = current_state->pump_power_watts_valid;
    if (current_state->pump_speed_valid &&
        (!s_discovery_published.pump || s_pump_discovery_with_power != with_power)) {
        mqtt_publish_pump_discovery_single(with_power);
        s_discovery_published.pump = true;
        s_pump_discovery_with_power = with_power;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/pump/state", device_id);

    char payload[128];
    int len = snprintf(payload, sizeof(payload), "{");

    // Pump speed
    if (current_state->pump_speed_valid) {
        len += snprintf(payload + len, sizeof(payload) - len,
                       "\"speed_rpm\":%d,", current_state->pump_speed);
    } else {
        len += snprintf(payload + len, sizeof(payload) - len, "\"speed_rpm\":null,");
    }
    
    // Pump power
    if (current_state->pump_power_watts_valid) {
        len += snprintf(payload + len, sizeof(payload) - len,
                       "\"power_watts\":%d}", current_state->pump_power_watts);
    } else {
        len += snprintf(payload + len, sizeof(payload) - len, "\"power_watts\":null}");
    }

    mqtt_publish(topic, payload, 0, true);

    // Update last published state
    s_last_published_state.pump_speed = current_state->pump_speed;
    s_last_published_state.pump_speed_valid = current_state->pump_speed_valid;
    s_last_published_state.pump_power_watts = current_state->pump_power_watts;
    s_last_published_state.pump_power_watts_valid = current_state->pump_power_watts_valid;

    ESP_LOGI(TAG, "Published pump: speed_rpm=%d", current_state->pump_speed);
}

void mqtt_publish_pump_energy(double energy_kwh)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/pump/energy/state", device_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "energy_kwh", energy_kwh);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(topic, payload, 0, true);
        free(payload);
    }
}

// ======================================================
// Service Mode Publishing
// ======================================================

void mqtt_publish_service_mode(const pool_state_t *current_state)
{
    // Check if anything changed
    if (s_last_published_state.service_mode_valid == current_state->service_mode_valid &&
        s_last_published_state.service_mode == current_state->service_mode) {
        return;  // No change, skip publish
    }

    // Publish discovery on first valid reading
    if (current_state->service_mode_valid && !s_discovery_published.service_mode) {
        mqtt_publish_service_mode_discovery_single();
        s_discovery_published.service_mode = true;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/service_mode/state", device_id);

    const char *payload = current_state->service_mode ? "ON" : "OFF";
    mqtt_publish(topic, payload, 0, true);

    // Update last published state
    s_last_published_state.service_mode = current_state->service_mode;
    s_last_published_state.service_mode_valid = current_state->service_mode_valid;

    ESP_LOGI(TAG, "Published service mode: %s", payload);
}

// ======================================================
// Valve Publishing
// ======================================================

void mqtt_publish_valve(const pool_state_t *current_state, uint8_t valve_num)
{
    if (valve_num < 1 || valve_num > MAX_VALVE_SLOTS) {
        return;
    }

    int idx = valve_num - 1;
    const valve_state_t *valve = &current_state->valves[idx];

    if (!valve->configured) {
        ESP_LOGI(TAG, "Skipping unconfigured valve %d", valve_num);
        return;
    }

    // Re-publish discovery if the valve name has changed since last publish
    if (s_discovery_published.valves[idx] &&
        strcmp(s_last_published_state.valves[idx].name, valve->name) != 0) {
        s_discovery_published.valves[idx] = false;
    }

    // Publish discovery if this is the first time seeing this valve (or name changed)
    if (!s_discovery_published.valves[idx]) {
        mqtt_publish_valve_discovery_single(valve_num, valve->name[0] != '\0' ? valve->name : NULL);
        s_discovery_published.valves[idx] = true;
    }

    // Check if anything changed
    if (s_last_published_state.valves[idx].configured &&
        s_last_published_state.valves[idx].state == valve->state &&
        s_last_published_state.valves[idx].active == valve->active &&
        strcmp(s_last_published_state.valves[idx].name, valve->name) == 0) {
        return;  // No change, skip publish
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/valve/%d/state", device_id, valve_num);

    static const char *STATE_NAMES[] = {"Off", "Auto", "On"};
    const char *state_name = (valve->state < 3) ? STATE_NAMES[valve->state] : "Unknown";

    const char *display_name = (valve->name[0] != '\0') ? valve->name : NULL;
    char fallback_name[16];
    if (!display_name) {
        snprintf(fallback_name, sizeof(fallback_name), "Valve %d", valve_num);
        display_name = fallback_name;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state",  state_name);
    cJSON_AddBoolToObject(root,   "active", valve->active);
    cJSON_AddStringToObject(root, "name",   display_name);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload) {
        mqtt_publish(topic, payload, 0, true);
        free(payload);
    }

    // Update last published state
    s_last_published_state.valves[idx].state = valve->state;
    s_last_published_state.valves[idx].active = valve->active;
    strncpy(s_last_published_state.valves[idx].name, valve->name,
            sizeof(s_last_published_state.valves[idx].name) - 1);
    s_last_published_state.valves[idx].name[sizeof(s_last_published_state.valves[idx].name) - 1] = '\0';
    s_last_published_state.valves[idx].configured = true;

    ESP_LOGI(TAG, "Published valve %d: %s (%s)", valve_num, state_name, display_name);
}

// ======================================================
// Favourite Publishing
// ======================================================

// Map active_favourite value to the matching option name string.
// Uses the pool_state favourites[] to get the registered name where available.
static const char *get_favourite_option_name(const pool_state_t *state, uint8_t value,
                                             char *buf, size_t buf_len)
{
    if (value == FAVOURITE_ALL_OFF) {
        return "All Off";
    }
    if (value == FAVOURITE_ALL_AUTO) {
        return "All Auto";
    }
    if (value == FAVOURITE_NONE) {
        // Not "None": HA treats a payload of literally "None" as its
        // set-to-unknown sentinel, which blanks the select
        return "No Favourite";
    }
    if (value < MAX_FAVOURITES) {
        if (state->favourites[value].name_valid && state->favourites[value].name[0] != '\0') {
            return state->favourites[value].name;
        }
        // Fallback names
        if (value == FAVOURITE_POOL) return "Pool";
        if (value == FAVOURITE_SPA)  return "Spa";
        snprintf(buf, buf_len, "Favourite %d", value - 1);
        return buf;
    }
    snprintf(buf, buf_len, "Unknown (0x%02X)", value);
    return buf;
}

void mqtt_publish_favourite(const pool_state_t *state)
{
    // Re-publish discovery if the options (favourites array) changed
    if (s_discovery_published.favourite) {
        for (int i = 0; i < MAX_FAVOURITES; i++) {
            const favourite_t *cur  = &state->favourites[i];
            const favourite_t *last = &s_last_published_state.favourites[i];
            if (cur->enabled       != last->enabled       ||
                cur->enabled_valid != last->enabled_valid ||
                cur->name_valid    != last->name_valid    ||
                strcmp(cur->name, last->name) != 0) {
                s_discovery_published.favourite = false;
                break;
            }
        }
    }

    if (!s_discovery_published.favourite) {
        mqtt_publish_favourite_discovery_single(state);
        s_discovery_published.favourite = true;
        // Update last-published favourites snapshot
        for (int i = 0; i < MAX_FAVOURITES; i++) {
            s_last_published_state.favourites[i] = state->favourites[i];
        }
    }

    // Skip state publish until we have an active favourite
    if (!state->active_favourite_valid) {
        return;
    }

    // Change-detect
    if (s_last_published_state.active_favourite_valid &&
        s_last_published_state.active_favourite == state->active_favourite) {
        return;
    }

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/favourite/state", device_id);

    char fallback[32];
    const char *name = get_favourite_option_name(state, state->active_favourite,
                                                 fallback, sizeof(fallback));
    mqtt_publish(topic, name, 0, true);

    s_last_published_state.active_favourite = state->active_favourite;
    s_last_published_state.active_favourite_valid = true;

    ESP_LOGI(TAG, "Published favourite: %s (0x%02X)", name, state->active_favourite);
}
