#include "web_handlers.h"
#include "pool_state.h"
#include "mqtt_poolclient.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WEB_HANDLERS";

// ======================================================
// Root Page Handler
// ======================================================

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ======================================================
// WiFi Scan Handler
// ======================================================

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    // Build JSON response
    char *json_resp = malloc(4096);
    if (json_resp == NULL) {
        free(ap_list);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int len = snprintf(json_resp, 4096, "[");
    for (int i = 0; i < ap_count && i < 20; i++) {
        len += snprintf(json_resp + len, 4096 - len,
                       "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                       i > 0 ? "," : "",
                       ap_list[i].ssid,
                       ap_list[i].rssi);
    }
    len += snprintf(json_resp + len, 4096 - len, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, len);

    free(ap_list);
    free(json_resp);
    return ESP_OK;
}

// ======================================================
// WiFi Provisioning Handler
// ======================================================

static esp_err_t provision_post_handler(httpd_req_t *req)
{
    char content[200];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Simple JSON parsing (looking for ssid and password fields)
    char ssid[33] = {0};
    char password[64] = {0};

    char *ssid_start = strstr(content, "\"ssid\":\"");
    char *pass_start = strstr(content, "\"password\":\"");

    if (ssid_start && pass_start) {
        ssid_start += 8; // Skip "ssid":"
        char *ssid_end = strchr(ssid_start, '"');
        if (ssid_end) {
            int ssid_len = ssid_end - ssid_start;
            if (ssid_len < sizeof(ssid)) {
                memcpy(ssid, ssid_start, ssid_len);
                ssid[ssid_len] = '\0';
            }
        }

        pass_start += 12; // Skip "password":"
        char *pass_end = strchr(pass_start, '"');
        if (pass_end) {
            int pass_len = pass_end - pass_start;
            if (pass_len < sizeof(password)) {
                memcpy(password, pass_start, pass_len);
                password[pass_len] = '\0';
            }
        }
    }

    if (strlen(ssid) == 0) {
        const char *resp = "{\"success\":false,\"message\":\"Invalid SSID\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Received WiFi credentials: SSID=%s", ssid);

    // Save credentials to NVS
    esp_err_t err = wifi_credentials_save(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials to NVS: %s", esp_err_to_name(err));
        const char *resp = "{\"success\":false,\"message\":\"Failed to save credentials\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Credentials saved to NVS successfully");

    // Send success response
    const char *resp = "{\"success\":true,\"message\":\"Connected! Device will restart...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    // Restart device to apply new credentials
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// ======================================================
// Pool Status Handler
// ======================================================

static esp_err_t status_get_handler(httpd_req_t *req)
{
    // Allocate a buffer for JSON response (8KB should be enough)
    char *json_resp = malloc(8192);
    if (json_resp == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int len = 0;

    // Lock the pool state and build JSON response
    if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Start JSON object
        len += snprintf(json_resp + len, 8192 - len, "{");

        // Temperature section
        len += snprintf(json_resp + len, 8192 - len, "\"temperature\":{");
        if (s_pool_state.temp_valid) {
            len += snprintf(json_resp + len, 8192 - len, "\"current\":%d,", s_pool_state.current_temp);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "\"current\":null,");
        }
        len += snprintf(json_resp + len, 8192 - len, "\"pool_setpoint\":%d,", s_pool_state.pool_setpoint);
        len += snprintf(json_resp + len, 8192 - len, "\"spa_setpoint\":%d,", s_pool_state.spa_setpoint);
        len += snprintf(json_resp + len, 8192 - len, "\"scale\":\"%s\"",
                       s_pool_state.temp_scale_fahrenheit ? "Fahrenheit" : "Celsius");
        len += snprintf(json_resp + len, 8192 - len, "},");

        // Heater section
        len += snprintf(json_resp + len, 8192 - len, "\"heater\":{");
        if (s_pool_state.heater_valid) {
            len += snprintf(json_resp + len, 8192 - len, "\"state\":\"%s\"",
                           s_pool_state.heater_on ? "on" : "off");
        } else {
            len += snprintf(json_resp + len, 8192 - len, "\"state\":null");
        }
        len += snprintf(json_resp + len, 8192 - len, "},");

        // Mode section
        len += snprintf(json_resp + len, 8192 - len, "\"mode\":");
        if (s_pool_state.mode_valid) {
            const char *mode_str = (s_pool_state.mode == 0) ? "Spa" :
                                   (s_pool_state.mode == 1) ? "Pool" : "Unknown";
            len += snprintf(json_resp + len, 8192 - len, "\"%s\",", mode_str);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "null,");
        }

        // Channels section
        len += snprintf(json_resp + len, 8192 - len, "\"channels\":[");
        bool first_channel = true;
        for (int i = 0; i < 8; i++) {
            if (s_pool_state.channels[i].configured) {
                if (!first_channel) {
                    len += snprintf(json_resp + len, 8192 - len, ",");
                }
                first_channel = false;

                const char *type_name = (s_pool_state.channels[i].type < CHANNEL_TYPE_COUNT) ?
                                       CHANNEL_TYPE_NAMES[s_pool_state.channels[i].type] : "Unknown";
                const char *state_name = (s_pool_state.channels[i].state < CHANNEL_STATE_COUNT) ?
                                        CHANNEL_STATE_NAMES[s_pool_state.channels[i].state] : "Unknown";

                len += snprintf(json_resp + len, 8192 - len,
                               "{\"id\":%d,\"name\":\"%s\",\"type\":\"%s\",\"state\":\"%s\"}",
                               s_pool_state.channels[i].id,
                               s_pool_state.channels[i].name[0] ? s_pool_state.channels[i].name : type_name,
                               type_name,
                               state_name);
            }
        }
        len += snprintf(json_resp + len, 8192 - len, "],");

        // Lighting section
        len += snprintf(json_resp + len, 8192 - len, "\"lighting\":[");
        bool first_light = true;
        for (int i = 0; i < 4; i++) {
            if (s_pool_state.lighting[i].configured) {
                if (!first_light) {
                    len += snprintf(json_resp + len, 8192 - len, ",");
                }
                first_light = false;

                const char *state_name = (s_pool_state.lighting[i].state < LIGHTING_STATE_COUNT) ?
                                        LIGHTING_STATE_NAMES[s_pool_state.lighting[i].state] : "Unknown";
                const char *color_name = (s_pool_state.lighting[i].color < LIGHTING_COLOR_COUNT) ?
                                        LIGHTING_COLOR_NAMES[s_pool_state.lighting[i].color] : "Unknown";

                len += snprintf(json_resp + len, 8192 - len,
                               "{\"zone\":%d,\"state\":\"%s\",\"color\":\"%s\",\"active\":%s}",
                               s_pool_state.lighting[i].zone,
                               state_name,
                               color_name,
                               s_pool_state.lighting[i].active ? "true" : "false");
            }
        }
        len += snprintf(json_resp + len, 8192 - len, "],");

        // Chlorinator section
        len += snprintf(json_resp + len, 8192 - len, "\"chlorinator\":{");
        if (s_pool_state.ph_valid) {
            len += snprintf(json_resp + len, 8192 - len, "\"ph_setpoint\":%.1f,",
                           s_pool_state.ph_setpoint / 10.0);
            len += snprintf(json_resp + len, 8192 - len, "\"ph_reading\":%.1f,",
                           s_pool_state.ph_reading / 10.0);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "\"ph_setpoint\":null,\"ph_reading\":null,");
        }
        if (s_pool_state.orp_valid) {
            len += snprintf(json_resp + len, 8192 - len, "\"orp_setpoint\":%d,",
                           s_pool_state.orp_setpoint);
            len += snprintf(json_resp + len, 8192 - len, "\"orp_reading\":%d",
                           s_pool_state.orp_reading);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "\"orp_setpoint\":null,\"orp_reading\":null");
        }
        len += snprintf(json_resp + len, 8192 - len, "},");

        // Last update timestamp
        len += snprintf(json_resp + len, 8192 - len, "\"last_update_ms\":%lu",
                       (unsigned long)s_pool_state.last_update_ms);

        // Close JSON object
        len += snprintf(json_resp + len, 8192 - len, "}");

        xSemaphoreGive(s_pool_state_mutex);
    } else {
        free(json_resp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to acquire state mutex");
        return ESP_FAIL;
    }

    // Send JSON response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, len);

    free(json_resp);
    return ESP_OK;
}

// ======================================================
// MQTT Configuration Handlers
// ======================================================

static esp_err_t mqtt_config_get_handler(httpd_req_t *req)
{
    // Load current config
    mqtt_config_t config = {0};
    mqtt_load_config(&config);

    const char *html_start =
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
        "h1{color:#333}.container{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);max-width:500px}"
        "label{display:block;margin:15px 0 5px;font-weight:bold;color:#555}"
        "input[type=text],input[type=number],input[type=password]{width:100%;padding:8px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}"
        "input[type=checkbox]{margin-right:10px}"
        ".checkbox-label{display:inline;font-weight:normal}"
        "button{background:#4CAF50;color:white;padding:12px 30px;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin-top:20px}"
        "button:hover{background:#45a049}"
        ".status{margin-top:15px;padding:10px;border-radius:4px;display:none}"
        ".success{background:#d4edda;color:#155724;border:1px solid #c3e6cb}"
        ".error{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}"
        "</style></head><body><div class='container'>"
        "<h1>MQTT Configuration</h1>"
        "<form id='mqttForm'>"
        "<label><input type='checkbox' id='enabled' name='enabled'";

    char checkbox_checked[16] = "";
    if (config.enabled) {
        strncpy(checkbox_checked, " checked", sizeof(checkbox_checked) - 1);
    }

    char html_mid[1024];
    snprintf(html_mid, sizeof(html_mid),
        "%s><span class='checkbox-label'>Enable MQTT</span></label>"
        "<label>Broker Host/IP:<input type='text' id='broker' name='broker' value='%s' placeholder='mqtt.example.com'></label>"
        "<label>Port:<input type='number' id='port' name='port' value='%d' min='1' max='65535'></label>"
        "<label>Username (optional):<input type='text' id='username' name='username' value='%s' placeholder='Leave empty if not required'></label>"
        "<label>Password (optional):<input type='password' id='password' name='password' value='%s' placeholder='Leave empty if not required'></label>"
        "<button type='submit'>Save Configuration</button>"
        "<div id='status' class='status'></div></form>",
        checkbox_checked, config.broker, config.port, config.username, config.password);

    const char *html_end =
        "<script>"
        "document.getElementById('mqttForm').addEventListener('submit',function(e){"
        "e.preventDefault();"
        "const data={enabled:document.getElementById('enabled').checked,"
        "broker:document.getElementById('broker').value,"
        "port:parseInt(document.getElementById('port').value)||1883,"
        "username:document.getElementById('username').value,"
        "password:document.getElementById('password').value};"
        "fetch('/mqtt_config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})"
        ".then(r=>r.json()).then(d=>{"
        "const s=document.getElementById('status');"
        "s.textContent=d.message;"
        "s.className='status '+(d.success?'success':'error');"
        "s.style.display='block';"
        "if(d.success)setTimeout(()=>window.location.reload(),2000);"
        "}).catch(()=>{"
        "const s=document.getElementById('status');"
        "s.textContent='Failed to save configuration';"
        "s.className='status error';s.style.display='block';});"
        "});"
        "</script></div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, html_start, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_mid, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_end, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t mqtt_config_post_handler(httpd_req_t *req)
{
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Simple JSON parsing
    mqtt_config_t config = {0};
    config.port = 1883;  // Default port

    char *enabled_start = strstr(content, "\"enabled\":");
    if (enabled_start) {
        enabled_start += 10;
        config.enabled = (strncmp(enabled_start, "true", 4) == 0);
    }

    char *broker_start = strstr(content, "\"broker\":\"");
    if (broker_start) {
        broker_start += 10;
        char *broker_end = strchr(broker_start, '"');
        if (broker_end) {
            int broker_len = broker_end - broker_start;
            if (broker_len < sizeof(config.broker)) {
                memcpy(config.broker, broker_start, broker_len);
                config.broker[broker_len] = '\0';
            }
        }
    }

    char *port_start = strstr(content, "\"port\":");
    if (port_start) {
        port_start += 7;
        config.port = atoi(port_start);
    }

    char *username_start = strstr(content, "\"username\":\"");
    if (username_start) {
        username_start += 12;
        char *username_end = strchr(username_start, '"');
        if (username_end) {
            int username_len = username_end - username_start;
            if (username_len < sizeof(config.username)) {
                memcpy(config.username, username_start, username_len);
                config.username[username_len] = '\0';
            }
        }
    }

    char *password_start = strstr(content, "\"password\":\"");
    if (password_start) {
        password_start += 12;
        char *password_end = strchr(password_start, '"');
        if (password_end) {
            int password_len = password_end - password_start;
            if (password_len < sizeof(config.password)) {
                memcpy(config.password, password_start, password_len);
                config.password[password_len] = '\0';
            }
        }
    }

    ESP_LOGI(TAG, "Received MQTT config: enabled=%d, broker=%s, port=%d",
             config.enabled, config.broker, config.port);

    // Save configuration
    esp_err_t err = mqtt_save_config(&config);
    if (err != ESP_OK) {
        const char *resp = "{\"success\":false,\"message\":\"Failed to save config\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Send success response
    const char *resp = "{\"success\":true,\"message\":\"MQTT config saved! Device will restart...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    // Restart device to apply new MQTT config
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// ======================================================
// URI Handlers
// ======================================================

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler
};

static const httpd_uri_t scan_uri = {
    .uri = "/scan",
    .method = HTTP_GET,
    .handler = scan_get_handler
};

static const httpd_uri_t provision_uri = {
    .uri = "/provision",
    .method = HTTP_POST,
    .handler = provision_post_handler
};

static const httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_get_handler
};

static const httpd_uri_t mqtt_config_get_uri = {
    .uri = "/mqtt_config",
    .method = HTTP_GET,
    .handler = mqtt_config_get_handler
};

static const httpd_uri_t mqtt_config_post_uri = {
    .uri = "/mqtt_config",
    .method = HTTP_POST,
    .handler = mqtt_config_post_handler
};

// ======================================================
// Public Functions
// ======================================================

esp_err_t web_handlers_register(httpd_handle_t server)
{
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &scan_uri);
    httpd_register_uri_handler(server, &provision_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &mqtt_config_get_uri);
    httpd_register_uri_handler(server, &mqtt_config_post_uri);
    ESP_LOGI(TAG, "Web/HTTP handlers registered");
    return ESP_OK;
}

