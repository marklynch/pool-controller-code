#ifndef POOL_STATE_H
#define POOL_STATE_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Channel types
#define CHANNEL_TYPE_COUNT 19
extern const char *CHANNEL_TYPE_NAMES[];

// Channel states
#define CHANNEL_STATE_COUNT 6
extern const char *CHANNEL_STATE_NAMES[];

// Lighting states
#define LIGHTING_STATE_COUNT 3
extern const char *LIGHTING_STATE_NAMES[];

// Lighting colors
#define LIGHTING_COLOR_COUNT 51
extern const char *LIGHTING_COLOR_NAMES[];

// Struct definitions
typedef struct {
    uint8_t id;
    char name[32];
    uint8_t type;
    uint8_t state;
    bool configured;
} channel_state_t;

typedef struct {
    uint8_t zone;
    uint8_t state;
    uint8_t color;
    bool active;
    bool configured;
} lighting_state_t;

// Register label storage entry
typedef struct {
    uint8_t reg_id;      // Register identifier (byte 10 from message)
    char label[32];      // Label string
    bool valid;          // True if this entry has been populated
} register_label_t;

typedef struct {
    // Temperature
    uint8_t current_temp;
    uint8_t pool_setpoint;
    uint8_t spa_setpoint;
    uint8_t pool_setpoint_f;  // Fahrenheit setpoint
    uint8_t spa_setpoint_f;   // Fahrenheit setpoint
    bool temp_scale_fahrenheit;
    bool temp_valid;

    // Heater
    bool heater_on;
    bool heater_valid;

    // Mode
    uint8_t mode;  // 0=Spa, 1=Pool
    bool mode_valid;

    // Channels (up to 8)
    channel_state_t channels[8];
    uint8_t num_channels;

    // Lighting (up to 4 zones)
    lighting_state_t lighting[4];

    // Register labels (general storage for register names like favourites, etc.)
    register_label_t register_labels[32];  // Support up to 32 different register labels

    // Device serial number
    uint32_t serial_number;
    bool serial_number_valid;

    // Internet Gateway IP address
    uint8_t gateway_ip[4];  // IPv4 address bytes
    uint8_t gateway_signal_level;  // Signal level
    bool gateway_ip_valid;

    // Internet Gateway communications status
    uint16_t gateway_comms_status;
    bool gateway_comms_status_valid;

    // Chlorinator
    uint16_t ph_setpoint;      // pH * 10 (e.g., 74 = 7.4)
    uint16_t ph_reading;       // pH * 10
    uint16_t orp_setpoint;     // mV
    uint16_t orp_reading;      // mV
    bool ph_valid;
    bool orp_valid;

    // Controller time/clock
    uint8_t controller_seconds;
    uint8_t controller_minutes;
    uint8_t controller_hours;
    bool controller_time_valid;

    // Touchscreen firmware version
    uint8_t touchscreen_version_major;
    uint8_t touchscreen_version_minor;
    bool touchscreen_version_valid;

    // Last update timestamp (milliseconds since boot)
    uint32_t last_update_ms;
} pool_state_t;

// Global pool state (defined in main.c)
extern pool_state_t s_pool_state;
extern SemaphoreHandle_t s_pool_state_mutex;

#endif // POOL_STATE_H
