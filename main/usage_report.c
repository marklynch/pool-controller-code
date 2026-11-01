#include "usage_report.h"
#include "config.h"

#include "nvs_settings.h"
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
#define USAGE_REPORT_NVS_NAMESPACE      "usage"
#define USAGE_REPORT_NVS_KEY_ID         "id"
#define USAGE_REPORT_NVS_KEY_ENABLED    "enabled"
#define USAGE_REPORT_NVS_KEY_DEL_PENDING "del_pending"

#define USAGE_REPORT_ID_BYTES        8
#define USAGE_REPORT_ID_HEX_LEN      (USAGE_REPORT_ID_BYTES * 2)
#define USAGE_REPORT_BODY_LEN        96    // {"id":"<16 hex>","v":"<≤31 chars>"} fits with room to spare
#define USAGE_REPORT_UA_LEN          64    // "pool-controller-esp32/" + version
#define USAGE_REPORT_DELETE_URL_LEN  (sizeof(USAGE_REPORT_URL) + 1 + USAGE_REPORT_ID_HEX_LEN)

// Written from the httpd task (usage_report_set_enabled) and read from the
// firmware-update check task. Unsynchronised: an aligned bool is read and
// written atomically on this core. A run that catches the old value of
// s_enabled is at most one ping either side of the switch, and a ping that
// slips in after opting out is still caught by the deletion that follows,
// because s_delete_pending is set on every opt-out.
static bool s_enabled = USAGE_REPORT_DEFAULT_ON;

// The user opted out and the server still has to be asked to delete this
// install's rows. Persisted, so the request survives being offline or a
// reboot. Set by the httpd task; cleared by the check task once the server
// confirms, or when there is no ID to delete.
static bool s_delete_pending;

// Install ID as lowercase hex; empty until the first ping creates it. Only
// the check task creates or sends it (usage_report_init runs before that task
// starts), so it needs no lock. Kept across an opt-out.
static char s_id_hex[USAGE_REPORT_ID_HEX_LEN + 1];

static void id_to_hex(const uint8_t id[USAGE_REPORT_ID_BYTES])
{
    for (int i = 0; i < USAGE_REPORT_ID_BYTES; i++) {
        snprintf(&s_id_hex[i * 2], 3, "%02x", id[i]);
    }
}

// Make and store a new install ID. Leaves s_id_hex empty if it could not be
// stored: an ID that the next boot would replace would count this device as a
// new install every time it restarts, so no stored ID means no ping.
static void create_id(void)
{
    uint8_t id[USAGE_REPORT_ID_BYTES];
    esp_fill_random(id, sizeof(id));

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(USAGE_REPORT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, USAGE_REPORT_NVS_KEY_ID, id, sizeof(id));
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
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
    s_delete_pending = false;
    s_id_hex[0] = '\0';

    uint8_t stored;
    if (nvs_settings_get_u8(USAGE_REPORT_NVS_NAMESPACE, USAGE_REPORT_NVS_KEY_ENABLED, &stored)) {
        s_enabled = (stored != 0);
    }
    if (nvs_settings_get_u8(USAGE_REPORT_NVS_NAMESPACE, USAGE_REPORT_NVS_KEY_DEL_PENDING, &stored)) {
        s_delete_pending = (stored != 0);
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(USAGE_REPORT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        uint8_t id[USAGE_REPORT_ID_BYTES];
        size_t len = sizeof(id);
        if (nvs_get_blob(nvs_handle, USAGE_REPORT_NVS_KEY_ID, id, &len) == ESP_OK &&
            len == sizeof(id)) {
            id_to_hex(id);
        }
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Anonymous usage reporting %s%s", s_enabled ? "enabled" : "disabled",
             s_delete_pending ? " (server deletion pending)" : "");
}

bool usage_report_get_enabled(void)
{
    return s_enabled;
}

void usage_report_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "Anonymous usage reporting %s", enabled ? "enabled" : "disabled");
    nvs_settings_set_u8(USAGE_REPORT_NVS_NAMESPACE, USAGE_REPORT_NVS_KEY_ENABLED, enabled ? 1 : 0);

    // Opting out asks for this install's rows to be deleted; opting back in
    // before that has happened withdraws the request.
    s_delete_pending = !enabled;
    nvs_settings_set_u8(USAGE_REPORT_NVS_NAMESPACE, USAGE_REPORT_NVS_KEY_DEL_PENDING, enabled ? 0 : 1);
}

// One request, no retries. Returns true on a 2xx response.
static bool send_request(esp_http_client_method_t method, const char *url, const char *body)
{
    const char *version = esp_app_get_description()->version;
    char user_agent[USAGE_REPORT_UA_LEN];
    snprintf(user_agent, sizeof(user_agent), "pool-controller-esp32/%s", version);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = USAGE_REPORT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGD(TAG, "request not sent: could not create HTTP client");
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", user_agent);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "request failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGD(TAG, "request rejected (HTTP %d)", status);
        return false;
    }
    return true;
}

static void delete_from_server(void)
{
    if (s_id_hex[0] != '\0') {
        char url[USAGE_REPORT_DELETE_URL_LEN];
        snprintf(url, sizeof(url), USAGE_REPORT_URL "/%s", s_id_hex);
        if (!send_request(HTTP_METHOD_DELETE, url, NULL)) {
            return;   // Still pending; the next run tries again
        }
        ESP_LOGI(TAG, "Usage data deleted from the server");
    }
    // Done, or there was never an ID and so nothing on the server.
    s_delete_pending = false;
    nvs_settings_set_u8(USAGE_REPORT_NVS_NAMESPACE, USAGE_REPORT_NVS_KEY_DEL_PENDING, 0);
}

static void ping(void)
{
    if (s_id_hex[0] == '\0') {
        create_id();
        if (s_id_hex[0] == '\0') {
            return;
        }
    }

    char body[USAGE_REPORT_BODY_LEN];
    snprintf(body, sizeof(body), "{\"id\":\"%s\",\"v\":\"%s\"}",
             s_id_hex, esp_app_get_description()->version);
    if (send_request(HTTP_METHOD_POST, USAGE_REPORT_URL, body)) {
        ESP_LOGI(TAG, "Usage report sent");
    }
}

void usage_report_run(bool ping_due)
{
    if (s_delete_pending) {
        delete_from_server();
    } else if (ping_due && s_enabled) {
        ping();
    }
}
