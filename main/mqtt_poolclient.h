#ifndef MQTT_POOLCLIENT_H
#define MQTT_POOLCLIENT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// MQTT configuration structure
typedef struct {
    bool enabled;
    char broker[128];
    uint16_t port;
    char username[64];
    char password[64];
} mqtt_config_t;

// MQTT client lifecycle functions
esp_err_t mqtt_client_init(void);
esp_err_t mqtt_client_start(void);
esp_err_t mqtt_client_stop(void);
bool mqtt_client_is_connected(void);

// Get device ID (generated from MAC address)
void mqtt_get_device_id(char *device_id, size_t max_len);

// Publish function (used by mqtt_publish.c)
esp_err_t mqtt_publish(const char *topic, const char *payload, int qos, bool retain);

// Load MQTT configuration from NVS
bool mqtt_load_config(mqtt_config_t *config);

// Save MQTT configuration to NVS
esp_err_t mqtt_save_config(const mqtt_config_t *config);

#endif // MQTT_POOLCLIENT_H
