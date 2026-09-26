#include "filter_pump_type.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "FILTER_PUMP";

// One namespace per module, as with channel_pwr and mqtt_config
#define FILTER_PUMP_TYPE_NVS_NAMESPACE "filter_pump"
#define FILTER_PUMP_TYPE_NVS_KEY       "type"

// Written from the decoder task (filter_pump_type_learn, off a CMD 0x0B
// broadcast) and read from the MQTT task (filter_pump_type_get, when building
// the pump-mode select's option list). Unsynchronised: an aligned enum is
// read and written atomically on this core, and a reader that catches the old
// value gets one stale option list, corrected by the next state publish.
static filter_pump_type_t s_type = FILTER_PUMP_TYPE_UNKNOWN;

static const char *type_name(filter_pump_type_t type)
{
    switch (type) {
        case FILTER_PUMP_TYPE_SINGLE_SPEED: return "single-speed";
        case FILTER_PUMP_TYPE_MULTI_SPEED:  return "multi-speed";
        default:                return "unknown";
    }
}

void filter_pump_type_init(void)
{
    // What is stored — or its absence — is the answer.
    s_type = FILTER_PUMP_TYPE_UNKNOWN;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(FILTER_PUMP_TYPE_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No stored filter pump type yet");
        return;
    }

    uint8_t stored = FILTER_PUMP_TYPE_UNKNOWN;
    if (nvs_get_u8(nvs_handle, FILTER_PUMP_TYPE_NVS_KEY, &stored) == ESP_OK &&
        stored <= FILTER_PUMP_TYPE_MULTI_SPEED) {
        s_type = (filter_pump_type_t)stored;
    }
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Filter pump type: %s", type_name(s_type));
}

filter_pump_type_t filter_pump_type_get(void)
{
    return s_type;
}

bool filter_pump_type_learn(uint8_t channel_state)
{
    filter_pump_type_t observed;
    switch (channel_state) {
        case 0x02:  // On — only single-speed channels use it
            observed = FILTER_PUMP_TYPE_SINGLE_SPEED;
            break;
        case 0x03:  // Low
        case 0x04:  // Medium
        case 0x05:  // High
            observed = FILTER_PUMP_TYPE_MULTI_SPEED;
            break;
        default:    // Off and Auto are common to both
            return false;
    }

    if (observed == s_type) {
        return false;
    }

    // The newest definitive evidence wins, so a pump swap corrects itself
    // without the flash needing to be cleared.
    ESP_LOGI(TAG, "Filter pump type: %s -> %s (from channel state 0x%02X)",
             type_name(s_type), type_name(observed), channel_state);
    s_type = observed;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(FILTER_PUMP_TYPE_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        // The in-RAM answer still stands for this boot; only persistence is lost.
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return true;
    }
    err = nvs_set_u8(nvs_handle, FILTER_PUMP_TYPE_NVS_KEY, (uint8_t)s_type);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store filter pump type: %s", esp_err_to_name(err));
    }
    nvs_close(nvs_handle);

    return true;
}
