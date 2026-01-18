#ifndef MQTT_COMMANDS_H
#define MQTT_COMMANDS_H

#include <stdint.h>

// Handle incoming MQTT command
// This function parses the topic and payload and sends appropriate UART commands
void mqtt_handle_command(const char *topic, int topic_len, const char *data, int data_len);

#endif // MQTT_COMMANDS_H
