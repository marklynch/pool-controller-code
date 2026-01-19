#ifndef MQTT_DISCOVERY_H
#define MQTT_DISCOVERY_H

// Publish all Home Assistant discovery messages
// This should be called once when MQTT connects
void mqtt_publish_discovery(void);

// Publish individual channel discovery (called when channel first configured)
void mqtt_publish_channel_discovery_single(int channel_num, const char *channel_name);

// Publish individual light discovery (called when light first configured)
void mqtt_publish_light_discovery_single(int zone_num);

#endif // MQTT_DISCOVERY_H
