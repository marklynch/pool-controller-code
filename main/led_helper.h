#ifndef LED_HELPER_H
#define LED_HELPER_H

#include "esp_err.h"

// LED persistent states
typedef enum {
    LED_STATE_STARTUP,           // Blue - brief boot indication
    LED_STATE_UNCONFIGURED,      // Purple - needs WiFi setup
    LED_STATE_WIFI_ONLY,         // White - WiFi connected, waiting for MQTT
    LED_STATE_FULLY_CONNECTED,   // Green - WiFi + MQTT operational
    LED_STATE_MQTT_DISCONNECTED, // Orange - WiFi ok, MQTT issue
} led_persistent_state_t;

// Initialize the LED strip
esp_err_t led_init(void);

// Set LED to startup color (blue)
void led_set_startup(void);

// Flash LED when receiving from UART (cyan flash)
void led_flash_rx(void);

// Flash LED when transmitting to UART (magenta flash)
void led_flash_tx(void);

// Set LED to unconfigured state (purple)
void led_set_unconfigured(void);

// Set LED to WiFi connected state (white) - MQTT not yet connected
void led_set_connected(void);

// Set LED to fully connected state (green) - WiFi + MQTT both connected
void led_set_mqtt_connected(void);

// Set LED to MQTT disconnected state (orange) - WiFi ok, MQTT issue
void led_set_mqtt_disconnected(void);

#endif // LED_HELPER_H
