/**
 * Mock mqtt_poolclient.h for host-based testing
 */
#ifndef MQTT_POOLCLIENT_H
#define MQTT_POOLCLIENT_H

#include <string.h>
#include <stddef.h>

void mqtt_get_device_id(char *device_id, size_t max_len);

#endif // MQTT_POOLCLIENT_H
