#ifndef MQTT_PUBLISH_H
#define MQTT_PUBLISH_H

#include <stdint.h>
#include <stdbool.h>

// Publish temperature data (current, pool setpoint, spa setpoint)
void mqtt_publish_temperature(uint8_t current_temp, uint8_t pool_setpoint, uint8_t spa_setpoint, bool temp_scale_fahrenheit);

// Publish heater state
void mqtt_publish_heater(bool heater_on);

// Publish mode (Pool/Spa)
void mqtt_publish_mode(uint8_t mode);

// Publish channel state by ID (1-8)
void mqtt_publish_channel(uint8_t channel_id, uint8_t type, uint8_t state, const char *name);

// Publish light zone state (1-4)
void mqtt_publish_light(uint8_t zone, uint8_t state, uint8_t color, bool active);

// Publish chlorinator data (pH and ORP)
void mqtt_publish_chlorinator(uint16_t ph_reading, uint16_t orp_reading, bool ph_valid, bool orp_valid);

#endif // MQTT_PUBLISH_H
