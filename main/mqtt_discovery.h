#ifndef MQTT_DISCOVERY_H
#define MQTT_DISCOVERY_H

// Publish all Home Assistant discovery messages
// This should be called once when MQTT connects
void mqtt_publish_discovery(void);

// Publish individual channel discovery (called when channel first configured)
void mqtt_publish_channel_discovery_single(int channel_num, const char *channel_name);

// Publish individual light discovery (called when light first configured or name changes)
void mqtt_publish_light_discovery_single(int zone_num, const char *zone_name);

// Publish individual valve discovery (called when valve first configured or name changes)
void mqtt_publish_valve_discovery_single(int valve_num, const char *valve_name);

#endif // MQTT_DISCOVERY_H
