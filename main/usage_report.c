#include "usage_report.h"
#include "config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "USAGE_REPORT";

// One namespace per module, as with filter_pump and channel_pwr
#define USAGE_REPORT_NVS_NAMESPACE   "usage"
#define USAGE_REPORT_NVS_KEY_ID      "id"
#define USAGE_REPORT_NVS_KEY_ENABLED "enabled"

#define USAGE_REPORT_ID_BYTES        8
#define USAGE_REPORT_ID_HEX_LEN      (USAGE_REPORT_ID_BYTES * 2)
#define USAGE_REPORT_BODY_LEN        96    // {"id":"<16 hex>","v":"<≤31 chars>"} fits with room to spare
#define USAGE_REPORT_UA_LEN          64    // "pool-controller-esp32/" + version

// Written from the httpd task (usage_report_set_enabled) and read from the
// firmware-update check task. Unsynchronised: an aligned bool is read and
// written atomically on this core, and a check that catches the old value is
// at most one ping either side of the switch.
static bool s_enabled = USAGE_REPORT_DEFAULT_ON;

// Install ID as lowercase hex. Empty when it could not be stored: sending an
// ID that the next boot would replace would count this device as a new
// install every time it restarts, so no ID means no ping.
static char s_id_hex[USAGE_REPORT_ID_HEX_LEN + 1];

static void id_to_hex(const uint8_t id[USAGE_REPORT_ID_BYTES])
{
    for (int i = 0; i < USAGE_REPORT_ID_BYTES; i++) {
        snprintf(&s_id_hex[i * 2], 3, "%02x", id[i]);
    }
}

// Read the stored ID, or make and store a new one. Leaves s_id_hex empty if
// no ID could be persisted.
static void load_or_create_id(nvs_handle_t nvs_handle)
{
    uint8_t id[USAGE_REPORT_ID_BYTES];
    size_t len = sizeof(id);
    if (nvs_get_blob(nvs_handle, USAGE_REPORT_NVS_KEY_ID, id, &len) == ESP_OK &&
        len == sizeof(id)) {
        id_to_hex(id);
        return;
    }

    esp_fill_random(id, sizeof(id));
    esp_err_t err = nvs_set_blob(nvs_handle, USAGE_REPORT_NVS_KEY_ID, id, sizeof(id));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store install ID: %s", esp_err_to_name(err));
        return;
    }
    id_to_hex(id);
    ESP_LOGI(TAG, "Created install ID");
}

void usage_report_init(void)
{
    s_enabled = USAGE_REPORT_DEFAULT_ON;
    s_id_hex[0] = '\0';

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(USAGE_REPORT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    uint8_t stored;
    if (nvs_get_u8(nvs_handle, USAGE_REPORT_NVS_KEY_ENABLED, &stored) == ESP_OK) {
        s_enabled = (stored != 0);
    }
    load_or_create_id(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Anonymous usage reporting %s", s_enabled ? "enabled" : "disabled");
}

bool usage_report_get_enabled(void)
{
    return s_enabled;
}

void usage_report_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "Anonymous usage reporting %s", enabled ? "enabled" : "disabled");

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(USAGE_REPORT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        // The setting still applies for this boot; only persistence is lost.
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs_handle, USAGE_REPORT_NVS_KEY_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store setting: %s", esp_err_to_name(err));
    }
    nvs_close(nvs_handle);
}

// One POST, no retries. Returns true on a 2xx response.
static bool send_ping(void)
{
    const char *version = esp_app_get_description()->version;

    char body[USAGE_REPORT_BODY_LEN];
    snprintf(body, sizeof(body), "{\"id\":\"%s\",\"v\":\"%s\"}", s_id_hex, version);

    char user_agent[USAGE_REPORT_UA_LEN];
    snprintf(user_agent, sizeof(user_agent), "pool-controller-esp32/%s", version);

    esp_http_client_config_t cfg = {
        .url = USAGE_REPORT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = USAGE_REPORT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGD(TAG, "ping not sent: could not create HTTP client");
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", user_agent);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "ping failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGD(TAG, "ping rejected (HTTP %d)", status);
        return false;
    }
    return true;
}

void usage_report_send(void)
{
    if (!s_enabled || s_id_hex[0] == '\0') {
        return;
    }

    if (send_ping()) {
        ESP_LOGI(TAG, "Usage report sent");
    }
}
