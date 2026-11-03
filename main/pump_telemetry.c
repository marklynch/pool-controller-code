#include "pump_telemetry.h"
#include "nvs_settings.h"
#include "esp_log.h"

static const char *TAG = "PUMP_TELEMETRY";

#define PUMP_TELEMETRY_NVS_NAMESPACE "pump_telemetry"
#define PUMP_TELEMETRY_NVS_KEY       "seen"

bool pump_telemetry_load_seen(void)
{
    uint8_t stored;
    bool seen = nvs_settings_get_u8(PUMP_TELEMETRY_NVS_NAMESPACE, PUMP_TELEMETRY_NVS_KEY, &stored) &&
                stored != 0;
    ESP_LOGI(TAG, "Pump telemetry %s", seen ? "seen on this install" : "not seen yet");
    return seen;
}

void pump_telemetry_save_seen(void)
{
    nvs_settings_set_u8(PUMP_TELEMETRY_NVS_NAMESPACE, PUMP_TELEMETRY_NVS_KEY, 1);
}
