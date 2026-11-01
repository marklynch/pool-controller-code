#include "nvs_settings.h"

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "NVS_SETTINGS";

bool nvs_settings_get_u8(const char *ns, const char *key, uint8_t *out)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(ns, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;   // Namespace not created yet: nothing stored
    }
    uint8_t stored;
    bool found = (nvs_get_u8(nvs_handle, key, &stored) == ESP_OK);
    nvs_close(nvs_handle);
    if (found) {
        *out = stored;
    }
    return found;
}

void nvs_settings_set_u8(const char *ns, const char *key, uint8_t value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store %s/%s: %s", ns, key, esp_err_to_name(err));
    }
}
