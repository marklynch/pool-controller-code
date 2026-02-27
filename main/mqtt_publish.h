#ifndef MQTT_PUBLISH_H
#define MQTT_PUBLISH_H

#include <stdint.h>
#include <stdbool.h>
#include "pool_state.h"

// Publish temperature data from pool state
void mqtt_publish_temperature(const pool_state_t *current_state);

// Publish heater state from pool state
void mqtt_publish_heater(const pool_state_t *current_state);

// Publish mode (Pool/Spa) from pool state
void mqtt_publish_mode(const pool_state_t *current_state);

// Publish channel state by ID (1-8) from pool state
void mqtt_publish_channel(const pool_state_t *current_state, uint8_t channel_id);

// Publish light zone state (1-4) from pool state
void mqtt_publish_light(const pool_state_t *current_state, uint8_t zone);

// Publish chlorinator data (pH and ORP) from pool state
void mqtt_publish_chlorinator(const pool_state_t *current_state);

// Publish valve state by number (1-based) from pool state
void mqtt_publish_valve(const pool_state_t *current_state, uint8_t valve_num);

#endif // MQTT_PUBLISH_H
