#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/mqtt_publish.h"
#include "../main/mqtt_poolclient.h"

static bool connected = true;
static unsigned count, discoveries;
static char topics[16][160], payloads[16][128];
bool mqtt_client_is_connected(void) { return connected; }
void mqtt_get_device_id(char *out, size_t size) { snprintf(out, size, "test"); }
const char *get_device_slug(uint8_t hi, uint8_t lo, char *out, size_t size)
{
    snprintf(out, size, "device_%02x%02x", hi, lo);
    return out;
}
esp_err_t mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    assert(count < 16 && qos == 0 && retain);
    snprintf(topics[count], sizeof(topics[count]), "%s", topic);
    snprintf(payloads[count++], sizeof(payloads[0]), "%s", payload);
    return ESP_OK;
}
void mqtt_publish_temperature_sensor_discovery_single(uint8_t hi, uint8_t lo,
                                                     uint8_t sensor, bool single)
{ discoveries++; }
void mqtt_publish_pump_discovery_single(bool with_power) { discoveries++; }

int main(void)
{
    static pool_state_t state;
    state.num_seen_devices = 1;
    seen_device_t *dev = &state.seen_devices[0];
    dev->addr_lo = 0x62;
    dev->has_temp_sensors = true;
    dev->temp1_connected = dev->temp2_connected = true;
    dev->temp1 = 24;
    dev->temp2 = 46;
    mqtt_publish_temperature_reading(&state, 0, 1);
    mqtt_publish_temperature_reading(&state, 0, 2);
    assert(count == 2 && discoveries == 2);
    assert(strstr(topics[0], "/1/state") && strstr(topics[1], "/2/state"));
    assert(strcmp(payloads[1], "{\"value\":46}") == 0);
    dev->temp1 = 25; dev->temp2 = 47;
    mqtt_publish_temperature_reading(&state, 0, 1);
    mqtt_publish_temperature_reading(&state, 0, 2);
    assert(count == 4 && strcmp(payloads[3], "{\"value\":47}") == 0);
    mqtt_publish_temperature_reading(&state, 0, 1);
    mqtt_publish_temperature_reading(&state, 0, 2);
    assert(count == 6 && discoveries == 2); /* Fresh unchanged samples. */
    /* Sensor not connected: one null, then nothing until it reconnects. */
    dev->temp2_connected = false;
    mqtt_publish_temperature_reading(&state, 0, 2);
    assert(count == 7 && strcmp(payloads[6], "{\"value\":null}") == 0);
    assert(strstr(topics[6], "/2/state"));
    mqtt_publish_temperature_reading(&state, 0, 2);
    mqtt_publish_temperature_reading(&state, -1, 1);
    mqtt_publish_temperature_reading(&state, 0, 3);
    assert(count == 7);
    dev->temp2_connected = true;
    mqtt_publish_temperature_reading(&state, 0, 2);
    assert(count == 8 && strcmp(payloads[7], "{\"value\":47}") == 0);

    /* A sensor never connected publishes null (clearing any stale retained
     * value) but gets no discovery. */
    state.num_seen_devices = 2;
    seen_device_t *unwired = &state.seen_devices[1];
    unwired->addr_lo = 0x74;
    unwired->has_temp_sensors = true;
    unwired->single_sensor_source = true;
    mqtt_publish_temperature_reading(&state, 1, 1);
    assert(count == 9 && discoveries == 2);
    assert(strcmp(payloads[8], "{\"value\":null}") == 0);

    state.pump_speed_valid = true; state.pump_speed = 2850;
    state.pump_power_watts_valid = true; state.pump_power_watts = 900;
    mqtt_publish_pump(&state);
    mqtt_publish_pump(&state);
    assert(count == 11 && discoveries == 3);
    assert(strcmp(payloads[9], payloads[10]) == 0);
    assert(strstr(payloads[10], "\"speed_rpm\":2850"));
    connected = false;
    mqtt_publish_temperature_reading(&state, 0, 1);
    mqtt_publish_pump(&state);
    assert(count == 11);
    puts("MQTT publisher: independent sensors, unchanged samples, not-connected nulls and disconnect passed");
    return 0;
}
