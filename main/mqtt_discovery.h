#ifndef MQTT_DISCOVERY_H
#define MQTT_DISCOVERY_H

// Publish all Home Assistant discovery messages
// This should be called once when MQTT connects
void mqtt_publish_discovery(void);

#endif // MQTT_DISCOVERY_H
