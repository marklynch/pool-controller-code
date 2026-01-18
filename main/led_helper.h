#ifndef LED_HELPER_H
#define LED_HELPER_H

#include "esp_err.h"

// Initialize the LED strip
esp_err_t led_init(void);

// Set LED to startup color (blue)
void led_set_startup(void);

// Flash LED when receiving from UART
void led_flash_rx(void);

// Flash LED when transmitting to UART
void led_flash_tx(void);

// Set LED to unconfigured state (purple)
void led_set_unconfigured(void);

// Set LED to connected state (yellow)
void led_set_connected(void);

// Set LED to MQTT connected state (cyan)
void led_set_mqtt_connected(void);

// Set LED to MQTT disconnected state (orange)
void led_set_mqtt_disconnected(void);

#endif // LED_HELPER_H
