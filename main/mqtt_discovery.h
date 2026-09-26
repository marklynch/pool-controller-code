#ifndef MQTT_DISCOVERY_H
#define MQTT_DISCOVERY_H

#include "pool_state.h"
#include <stdbool.h>

// Clear every retained discovery config belonging to this device and reboot,
// so Home Assistant deletes the entities and re-creates them from the configs
// published on the next connect. Entity ids are only assigned at first
// registration, so this is what applies a changed entity_id scheme — and it
// also clears entities left behind by earlier firmware. Returns immediately;
// the work runs on its own task and ends in a restart. False if it couldn't be
// started — MQTT not connected, or a reset already running.
bool mqtt_discovery_reset_start(void);

// Route an MQTT message to the entity reset, which subscribes to the broker's
// discovery configs while it runs. Returns true if the message belonged to the
// reset and must not be treated as a command. No-op when no reset is running.
bool mqtt_discovery_reset_handle_message(const char *topic, int topic_len, int data_len);

// Publish all Home Assistant discovery messages
// This should be called once when MQTT connects
void mqtt_publish_discovery(void);

// Publish the Home Assistant "update" entity discovery (firmware update via
// GitHub release). Published once on connect; state is carried on
// pool/<id>/update/state (see firmware_update.c).
void mqtt_publish_update_discovery_single(void);

// Publish individual channel discovery (called when channel first configured)
// include_state_entities: false for channel slots whose type is a Heater or
// Light Zone meta-type — those are controlled/reported through their own
// dedicated discovery (mqtt_publish_heater*/mqtt_publish_light), so the raw
// channel's state sensor, toggle button, and active binary_sensor would be
// redundant (and the toggle command isn't the correct way to control them
// anyway). The configured-power number is still published either way — it's
// how the user configures a wattage in the first place.
// include_power_sensors: whether this channel has a configured wattage to
// report from (channel_power_get_effective). False publishes a retraction for
// the power and energy sensors instead of a config, so they don't sit at
// unknown on a channel that can't report them; call again with true once one
// is configured.
// include_pump_mode_select adds the direct-set select (CMD 0x0F). Pump-driven
// channels act on that command — Filter and Cleaning are both confirmed (see
// PROTOCOL.md 0x0F) — but the select is offered on Filter alone, since only
// that channel's pump type is tracked.
void mqtt_publish_channel_discovery_single(int channel_num, const char *channel_name,
                                           bool include_state_entities, bool include_power_sensors,
                                           bool include_pump_mode_select);

// Publish the system baseline power entities (the "Power: System" number, plus
// the power and energy sensors when include_power_sensors is true — same
// gating as a channel's pair). Republished on every MQTT connect and whenever
// the baseline is changed, so it needs no first-seen tracking.
void mqtt_publish_system_power_discovery_single(bool include_power_sensors);

// Publish individual light discovery (called when light first configured or name changes)
// multicolor_light_type: MULTICOLOR_LIGHT_TYPE_* — pass MULTICOLOR_LIGHT_TYPE_NONE
// for non-multicolor zones; a known type adds the color effect list to the entity
void mqtt_publish_light_discovery_single(int zone_num, const char *zone_name, uint8_t multicolor_light_type);

// Publish individual valve discovery (called when valve first configured or name changes)
void mqtt_publish_valve_discovery_single(int valve_num, const char *valve_name);

// Publish individual heater discovery (called when heater first publishes state)
void mqtt_publish_heater_discovery_single(int index);

// Publish gas heater detail discovery: status sensor, water_flow/locked_out binary
// sensors, and burner sensor — all reading from pool/<id>/heater/<n>/gas_status/state.
void mqtt_publish_gas_heater_discovery_single(int index);

// Publish a heater's pool + spa setpoint Number entities (called when the heater
// first publishes setpoints).
void mqtt_publish_heater_setpoint_discovery_single(int index);

// Publish individual temperature-sensor discovery for a (source, sensor) pair.
// Called the first time `mqtt_publish_temperature_reading` fires for each
// (dev_idx, sensor_index). `single_sensor_source` selects the HA entity name
// and state topic shape — see mqtt_publish.c.
void mqtt_publish_temperature_sensor_discovery_single(
    uint8_t addr_hi, uint8_t addr_lo,
    uint8_t sensor_index, bool single_sensor_source);

// Publish favourite select discovery (called on connect and when names/enable flags change)
void mqtt_publish_favourite_discovery_single(const pool_state_t *state);

// Chemistry and pump sensor discovery, published lazily on each entity's
// first valid value (see mqtt_publish_chlorinator / mqtt_publish_pump).
void mqtt_publish_ph_discovery_single(void);
void mqtt_publish_orp_discovery_single(void);
void mqtt_publish_ph_setpoint_discovery_single(void);
void mqtt_publish_orp_setpoint_discovery_single(void);
void mqtt_publish_chlor_output_level_discovery_single(void);
void mqtt_publish_service_mode_discovery_single(void);

// Publish the pump entities: the speed sensor always, plus the power and
// energy sensors when include_power_sensors is true. The pump meters itself,
// so these are its own entities rather than being folded into the Filter
// channel that switches it — that channel's configured estimate covers
// whatever else shares the channel. False retracts the pair (same gating as a
// channel's), for a pump that only ever broadcasts speed-only telemetry.
void mqtt_publish_pump_discovery_single(bool include_power_sensors);

#endif // MQTT_DISCOVERY_H
