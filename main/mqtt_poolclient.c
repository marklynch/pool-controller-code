#include "mqtt_poolclient.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

// ESP-IDF MQTT component
#include <mqtt_client.h>

// Local headers
#include "device_serial.h"
#include "mqtt_discovery.h"
#include "mqtt_commands.h"
#include "led_helper.h"

static const char *TAG = "MQTT_CLIENT";

#define MQTT_NVS_NAMESPACE "mqtt_config"
#define NVS_KEY_ENABLED    "enabled"
#define NVS_KEY_BROKER     "broker"
#define NVS_KEY_PORT       "port"
#define NVS_KEY_USERNAME   "username"
#define NVS_KEY_PASSWORD   "password"

// Global MQTT client handle
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static char s_device_id[32] = {0};

// Forward declarations
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void publish_availability(bool online);

// ======================================================
// Device ID Generation
// ======================================================

void mqtt_get_device_id(char *device_id, size_t max_len)
{
    if (s_device_id[0] != '\0') {
        strncpy(device_id, s_device_id, max_len - 1);
        device_id[max_len - 1] = '\0';
        return;
    }

    char serial[DEVICE_SERIAL_LEN];
    device_get_serial(serial, sizeof(serial));
    snprintf(s_device_id, sizeof(s_device_id), "pool_%s", serial);
    strncpy(device_id, s_device_id, max_len - 1);
    device_id[max_len - 1] = '\0';
}

// ======================================================
// NVS Configuration Functions
// ======================================================

bool mqtt_load_config(mqtt_config_t *config)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT config not found in NVS");
        return false;
    }

    // Load enabled flag
    uint8_t enabled = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_ENABLED, &enabled);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }
    config->enabled = (enabled != 0);

    // Load broker
    size_t broker_len = sizeof(config->broker);
    err = nvs_get_str(nvs_handle, NVS_KEY_BROKER, config->broker, &broker_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }

    // Load port
    err = nvs_get_u16(nvs_handle, NVS_KEY_PORT, &config->port);
    if (err != ESP_OK) {
        config->port = MQTT_DEFAULT_PORT;
    }

    // Load username (optional)
    size_t username_len = sizeof(config->username);
    err = nvs_get_str(nvs_handle, NVS_KEY_USERNAME, config->username, &username_len);
    if (err != ESP_OK) {
        config->username[0] = '\0';
    }

    // Load password (optional)
    size_t password_len = sizeof(config->password);
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, config->password, &password_len);
    if (err != ESP_OK) {
        config->password[0] = '\0';
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "MQTT config loaded: broker=%s, port=%d, enabled=%d",
             config->broker, config->port, config->enabled);
    return true;
}

esp_err_t mqtt_save_config(const mqtt_config_t *config)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    // Save enabled flag
    err = nvs_set_u8(nvs_handle, NVS_KEY_ENABLED, config->enabled ? 1 : 0);
    if (err != ESP_OK) goto error;

    // Save broker
    err = nvs_set_str(nvs_handle, NVS_KEY_BROKER, config->broker);
    if (err != ESP_OK) goto error;

    // Save port
    err = nvs_set_u16(nvs_handle, NVS_KEY_PORT, config->port);
    if (err != ESP_OK) goto error;

    // Save username
    err = nvs_set_str(nvs_handle, NVS_KEY_USERNAME, config->username);
    if (err != ESP_OK) goto error;

    // Save password
    err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, config->password);
    if (err != ESP_OK) goto error;

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) goto error;

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "MQTT config saved to NVS");
    return ESP_OK;

error:
    nvs_close(nvs_handle);
    ESP_LOGE(TAG, "Failed to save MQTT config: %s", esp_err_to_name(err));
    return err;
}

// ======================================================
// MQTT Event Handler
// ======================================================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_connected = true;
        led_set_mqtt_connected();

        // Subscribe to command topics
        char device_id[32];
        mqtt_get_device_id(device_id, sizeof(device_id));

        char topic[128];
        // Subscribe to channel commands (channels 1-MAX_CHANNELS)
        for (int i = 1; i <= MAX_CHANNELS; i++) {
            snprintf(topic, sizeof(topic), "pool/%s/channel/%d/set", device_id, i);
            esp_mqtt_client_subscribe(s_mqtt_client, topic, 0);
        }

        // Subscribe to light commands (zones 1-MAX_LIGHT_ZONES)
        for (int i = 1; i <= MAX_LIGHT_ZONES; i++) {
            snprintf(topic, sizeof(topic), "pool/%s/light/%d/set", device_id, i);
            esp_mqtt_client_subscribe(s_mqtt_client, topic, 0);
        }

        // Subscribe to heater command
        snprintf(topic, sizeof(topic), "pool/%s/heater/set", device_id);
        esp_mqtt_client_subscribe(s_mqtt_client, topic, 0);

        // Subscribe to mode command
        snprintf(topic, sizeof(topic), "pool/%s/mode/set", device_id);
        esp_mqtt_client_subscribe(s_mqtt_client, topic, 0);

        // Subscribe to temperature setpoint commands
        snprintf(topic, sizeof(topic), "pool/%s/temperature/pool/set", device_id);
        esp_mqtt_client_subscribe(s_mqtt_client, topic, 0);
        snprintf(topic, sizeof(topic), "pool/%s/temperature/spa/set", device_id);
        esp_mqtt_client_subscribe(s_mqtt_client, topic, 0);

        ESP_LOGI(TAG, "Subscribed to command topics");

        // Publish Home Assistant discovery messages
        mqtt_publish_discovery();

        // Publish availability online
        publish_availability(true);

        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        led_set_mqtt_disconnected();
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT data received: topic=%.*s, data=%.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);

        // Handle command
        mqtt_handle_command(event->topic, event->topic_len, event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "TCP transport error");
        }
        led_set_mqtt_disconnected();
        break;

    default:
        break;
    }
}

// ======================================================
// MQTT Client Lifecycle
// ======================================================

esp_err_t mqtt_client_init(void)
{
    mqtt_config_t config;
    if (!mqtt_load_config(&config)) {
        ESP_LOGW(TAG, "MQTT config not found, skipping initialization");
        return ESP_ERR_NOT_FOUND;
    }

    if (!config.enabled) {
        ESP_LOGI(TAG, "MQTT disabled in config");
        return ESP_ERR_INVALID_STATE;
    }

    // Generate device ID
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    // Build broker URI
    char broker_uri[192];
    snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", config.broker, config.port);

    // Build LWT topic
    char lwt_topic[128];
    snprintf(lwt_topic, sizeof(lwt_topic), "pool/%s/availability", device_id);

    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.client_id = device_id,
        .session.last_will = {
            .topic = lwt_topic,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = true,
        },
    };

    // Add authentication if configured
    if (config.username[0] != '\0') {
        mqtt_cfg.credentials.username = config.username;
        ESP_LOGI(TAG, "MQTT username configured: %s", config.username);
    } else {
        ESP_LOGI(TAG, "MQTT username: NONE (anonymous)");
    }
    if (config.password[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = config.password;
        ESP_LOGI(TAG, "MQTT password configured: %d chars", strlen(config.password));
    } else {
        ESP_LOGI(TAG, "MQTT password: NONE");
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    // Register event handler
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    ESP_LOGI(TAG, "MQTT client initialized: %s", broker_uri);
    return ESP_OK;
}

esp_err_t mqtt_client_start(void)
{
    if (s_mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

esp_err_t mqtt_client_stop(void)
{
    if (s_mqtt_client == NULL) {
        return ESP_OK;
    }

    s_mqtt_connected = false;

    esp_err_t err = esp_mqtt_client_stop(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop MQTT client: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "MQTT client stopped");
    return err;
}

bool mqtt_client_is_connected(void)
{
    return s_mqtt_connected;
}

// ======================================================
// Publishing Functions
// ======================================================

static void publish_availability(bool online)
{
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[128];
    snprintf(topic, sizeof(topic), "pool/%s/availability", device_id);

    const char *payload = online ? "online" : "offline";
    mqtt_publish(topic, payload, 1, true);
}

esp_err_t mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (s_mqtt_client == NULL || !s_mqtt_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published to %s: %s", topic, payload);
    return ESP_OK;
}
