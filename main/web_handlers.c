#include "web_handlers.h"
#include "pool_state.h"
#include "mqtt_poolclient.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WEB_HANDLERS";

// Forward declaration - defined in main.c
const char* get_gateway_comms_status_text(uint16_t code);

// ======================================================
// Common HTML Page Parts (Header and footer)
// ======================================================

// Dynamic functions for page header, navigation, and footer
char *get_page_header(const char *title) {
    const char *fmt = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
        "h1{color:#333}.container{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);max-width:500px}"
        "label{display:block;margin:15px 0 5px;font-weight:bold;color:#555}"
        "input[type=text],input[type=number],input[type=password]{width:100%%;padding:8px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}"
        "input[type=checkbox]{margin-right:10px}"
        ".checkbox-label{display:inline;font-weight:normal}"
        "button{background:#4CAF50;color:white;padding:12px 30px;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin-top:20px}"
        "button:hover{background:#45a049}"
        ".status{margin-top:15px;padding:10px;border-radius:4px;display:none}"
        ".success{background:#d4edda;color:#155724;border:1px solid #c3e6cb}"
        ".error{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}"
        "</style><title>%s</title></head><body>";
    // calculate the required size
    int n = snprintf(NULL, 0, fmt, title);
    if (n < 0) return NULL;

    // allocate memory for the header
    char *header = malloc((size_t)n + 1);
    if (!header) return NULL;

    snprintf(header, (size_t)n + 1, fmt, title);
    return header;
}

char *get_page_nav(const char page) {
    // TODO - Make 'page' indicate current page for highlighting
    const char *fmt = "<div class='nav'><a href='/'>WiFi Scan</a> | "
        "<a href='/mqtt_config'>MQTT Config</a> | "
        "<a href='/status_view'>Status</a> | "
        "<a href='/update'>Update</a>"
        "</div><hr>";
    // calculate the required size
    int n = snprintf(NULL, 0, fmt, page);
    if (n < 0) return NULL;

    // allocate memory for the header
    char *header = malloc((size_t)n + 1);
    if (!header) return NULL;

    snprintf(header, (size_t)n + 1, fmt, page);
    return header;
}

char *get_page_footer(void) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *fmt = "<hr><div style='text-align:center;color:#666;font-size:12px;margin-top:30px'>"
        "Firmware: %s | Project: %s | Built: %s %s"
        "</div></body></html>";

    // Calculate required size
    int n = snprintf(NULL, 0, fmt, app_desc->version, app_desc->project_name,
                     app_desc->date, app_desc->time);
    if (n < 0) return NULL;

    // Allocate memory
    char *footer = malloc((size_t)n + 1);
    if (!footer) return NULL;

    snprintf(footer, (size_t)n + 1, fmt, app_desc->version, app_desc->project_name,
             app_desc->date, app_desc->time);
    return footer;
}

// ======================================================
// Root Page Handler
// ======================================================

// HTML page for WiFi provisioning
const char WIFI_PAGE[] = "<div class='container'><h1>WiFi Configuration</h1>"
"<div id='status'></div>"
"<form id='wifiForm'><label>WiFi Network:</label>"
"<select id='ssid' name='ssid' required><option value=''>Scanning...</option></select>"
"<label>Password:</label><input type='password' id='password' name='password' required>"
"<button type='submit'>Connect</button></form></div>"
"<script>"
"function showStatus(msg,isError){const d=document.getElementById('status');"
"d.innerHTML='<div class=\"status '+(isError?'error':'success')+'\">'+msg+'</div>';}"
"function scanWiFi(){fetch('/scan').then(r=>r.json()).then(data=>{"
"const s=document.getElementById('ssid');s.innerHTML='';"
"data.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;"
"o.text=n.ssid+' ('+n.rssi+' dBm)'+(n.current?' - Current':'');"
"if(n.current)o.selected=true;s.appendChild(o);});}).catch(e=>{"
"showStatus('Scan failed: '+e,true);});}"
"document.getElementById('wifiForm').onsubmit=function(e){"
"e.preventDefault();const ssid=document.getElementById('ssid').value;"
"const pass=document.getElementById('password').value;"
"showStatus('Connecting to '+ssid+'...',false);"
"fetch('/provision',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:ssid,password:pass})}).then(r=>r.json()).then(data=>{"
"if(data.success){showStatus('Connected! Device will restart.',false);"
"}else{showStatus('Failed: '+data.message,true);}}).catch(e=>{"
"showStatus('Error: '+e,true);});return false;};"
"scanWiFi();</script>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char page_title[] = "WiFi Configuration";
    char *header = get_page_header(page_title);
    char *nav = get_page_nav('/');
    char *footer = get_page_footer();
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nav, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, WIFI_PAGE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, footer, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    free(footer);
    free(nav);
    free(header);
    return ESP_OK;
}

// ======================================================
// WiFi Scan Handler
// ======================================================

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    // Load current WiFi SSID from NVS to mark it in results
    char current_ssid[33] = {0};
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(PROV_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        size_t ssid_len = sizeof(current_ssid);
        err = nvs_get_str(nvs_handle, PROV_NVS_KEY_SSID, current_ssid, &ssid_len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded current SSID from NVS: '%s' (len=%d)", current_ssid, ssid_len);
        } else {
            ESP_LOGW(TAG, "Failed to read SSID from NVS: %s", esp_err_to_name(err));
        }
        nvs_close(nvs_handle);
    } else {
        ESP_LOGW(TAG, "Failed to open %s NVS: %s", PROV_NVS_NAMESPACE, esp_err_to_name(err));
    }

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

    // Deduplicate - track unique SSIDs and their best RSSI
    typedef struct {
        char ssid[33];
        int8_t rssi;
    } unique_ap_t;

    unique_ap_t *unique_aps = malloc(sizeof(unique_ap_t) * ap_count);
    if (unique_aps == NULL) {
        free(ap_list);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int unique_count = 0;
    for (int i = 0; i < ap_count; i++) {
        // Check if this SSID already exists in unique list
        bool found = false;
        for (int j = 0; j < unique_count; j++) {
            if (strcmp((char *)ap_list[i].ssid, unique_aps[j].ssid) == 0) {
                // Update RSSI if this one is stronger (less negative)
                if (ap_list[i].rssi > unique_aps[j].rssi) {
                    unique_aps[j].rssi = ap_list[i].rssi;
                }
                found = true;
                break;
            }
        }

        // If new SSID, add it
        if (!found && unique_count < ap_count) {
            strncpy(unique_aps[unique_count].ssid, (char *)ap_list[i].ssid, sizeof(unique_aps[unique_count].ssid) - 1);
            unique_aps[unique_count].ssid[32] = '\0';
            unique_aps[unique_count].rssi = ap_list[i].rssi;
            unique_count++;
        }
    }

    // Build JSON response from unique list
    char *json_resp = malloc(4096);
    if (json_resp == NULL) {
        free(ap_list);
        free(unique_aps);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int len = snprintf(json_resp, 4096, "[");
    for (int i = 0; i < unique_count && i < 20; i++) {
        bool is_current = (strcmp(unique_aps[i].ssid, current_ssid) == 0);
        ESP_LOGI(TAG, "Comparing '%s' with '%s': %s", unique_aps[i].ssid, current_ssid, is_current ? "MATCH" : "no match");
        len += snprintf(json_resp + len, 4096 - len,
                       "%s{\"ssid\":\"%s\",\"rssi\":%d,\"current\":%s}",
                       i > 0 ? "," : "",
                       unique_aps[i].ssid,
                       unique_aps[i].rssi,
                       is_current ? "true" : "false");
    }
    len += snprintf(json_resp + len, 4096 - len, "]");

    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, json_resp, len);

    free(ap_list);
    free(unique_aps);
    free(json_resp);
    return ESP_OK;
}

// ======================================================
// WiFi Provisioning Handler
// ======================================================


// ======================================================
// Custom HTTP Handlers for Web Provisioning UI
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
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Received WiFi credentials: SSID=%s", ssid);

    // Save credentials to NVS
    esp_err_t err = wifi_credentials_save(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials to NVS: %s", esp_err_to_name(err));
        const char *resp = "{\"success\":false,\"message\":\"Failed to save credentials\"}";
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Credentials saved to NVS successfully");

    // Send success response
    const char *resp = "{\"success\":true,\"message\":\"Connected! Device will restart...\"}";
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
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

    // Get firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();

    // Lock the pool state and build JSON response
    if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Start JSON object
        len += snprintf(json_resp + len, 8192 - len, "{");

        // Firmware version section
        len += snprintf(json_resp + len, 8192 - len, "\"firmware\":{");
        len += snprintf(json_resp + len, 8192 - len, "\"version\":\"%s\",", app_desc->version);
        len += snprintf(json_resp + len, 8192 - len, "\"project\":\"%s\",", app_desc->project_name);
        len += snprintf(json_resp + len, 8192 - len, "\"compile_time\":\"%s %s\",", app_desc->date, app_desc->time);
        len += snprintf(json_resp + len, 8192 - len, "\"idf_version\":\"%s\"", app_desc->idf_ver);
        len += snprintf(json_resp + len, 8192 - len, "},");

        // Temperature section
        len += snprintf(json_resp + len, 8192 - len, "\"temperature\":{");
        if (s_pool_state.temp_valid) {
            len += snprintf(json_resp + len, 8192 - len, "\"current\":%d,", s_pool_state.current_temp);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "\"current\":null,");
        }
        len += snprintf(json_resp + len, 8192 - len, "\"pool_setpoint\":%d,", s_pool_state.pool_setpoint);
        len += snprintf(json_resp + len, 8192 - len, "\"spa_setpoint\":%d,", s_pool_state.spa_setpoint);
        len += snprintf(json_resp + len, 8192 - len, "\"pool_setpoint_f\":%d,", s_pool_state.pool_setpoint_f);
        len += snprintf(json_resp + len, 8192 - len, "\"spa_setpoint_f\":%d,", s_pool_state.spa_setpoint_f);
        len += snprintf(json_resp + len, 8192 - len, "\"scale\":\"%s\"",
                       s_pool_state.temp_scale_fahrenheit ? "Fahrenheit" : "Celsius");
        len += snprintf(json_resp + len, 8192 - len, "},");

        // Heater section
        len += snprintf(json_resp + len, 8192 - len, "\"heater\":{");
        if (s_pool_state.heater_valid) {
            len += snprintf(json_resp + len, 8192 - len, "\"state\":\"%s\"",
                           s_pool_state.heater_on ? "On" : "Off");
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

        // Internet Gateway section
        len += snprintf(json_resp + len, 8192 - len, "\"internet_gateway\":{");

        // Serial number
        len += snprintf(json_resp + len, 8192 - len, "\"serial_number\":");
        if (s_pool_state.serial_number_valid) {
            len += snprintf(json_resp + len, 8192 - len, "%lu,", (unsigned long)s_pool_state.serial_number);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "null,");
        }

        // IP address
        len += snprintf(json_resp + len, 8192 - len, "\"ip\":");
        if (s_pool_state.gateway_ip_valid) {
            len += snprintf(json_resp + len, 8192 - len, "\"%d.%d.%d.%d\",",
                           s_pool_state.gateway_ip[0], s_pool_state.gateway_ip[1],
                           s_pool_state.gateway_ip[2], s_pool_state.gateway_ip[3]);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "null,");
        }

        // Signal level
        len += snprintf(json_resp + len, 8192 - len, "\"signal_level\":");
        if (s_pool_state.gateway_ip_valid) {
            len += snprintf(json_resp + len, 8192 - len, "%d,", s_pool_state.gateway_signal_level);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "null,");
        }

        // Comms status
        len += snprintf(json_resp + len, 8192 - len, "\"comms_status\":");
        if (s_pool_state.gateway_comms_status_valid) {
            len += snprintf(json_resp + len, 8192 - len, "%u,", s_pool_state.gateway_comms_status);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "null,");
        }

        // Comms status text
        len += snprintf(json_resp + len, 8192 - len, "\"comms_status_text\":");
        if (s_pool_state.gateway_comms_status_valid) {
            const char *status_text = get_gateway_comms_status_text(s_pool_state.gateway_comms_status);
            len += snprintf(json_resp + len, 8192 - len, "\"%s\"", status_text);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "null");
        }

        len += snprintf(json_resp + len, 8192 - len, "},");

        // Touchscreen section
        len += snprintf(json_resp + len, 8192 - len, "\"touchscreen\":{");
        len += snprintf(json_resp + len, 8192 - len, "\"version\":");
        if (s_pool_state.touchscreen_version_valid) {
            len += snprintf(json_resp + len, 8192 - len, "\"%d.%d\"",
                           s_pool_state.touchscreen_version_major,
                           s_pool_state.touchscreen_version_minor);
        } else {
            len += snprintf(json_resp + len, 8192 - len, "null");
        }
        len += snprintf(json_resp + len, 8192 - len, "},");

        // Last update timestamp
        len += snprintf(json_resp + len, 8192 - len, "\"last_update_ms\":%lu",
                       (unsigned long)s_pool_state.last_update_ms);

        // Current system time in ms
        uint64_t current_tick_count = xTaskGetTickCount() * portTICK_PERIOD_MS;
        len += snprintf(json_resp + len, 8192 - len, ",\"current_ms\":%lu",
                       (unsigned long)current_tick_count);

        // Close JSON object
        len += snprintf(json_resp + len, 8192 - len, "}");

        xSemaphoreGive(s_pool_state_mutex);
    } else {
        free(json_resp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to acquire state mutex");
        return ESP_FAIL;
    }

    // Send JSON response
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, json_resp, len);

    free(json_resp);
    return ESP_OK;
}

// ======================================================
// Status View Handler (HTML formatted view)
// ======================================================

static esp_err_t status_view_get_handler(httpd_req_t *req)
{
    const char *status_view_page =
        "<div class='container'>"
        "<h1>Pool Controller Status</h1>"
        "<p style='text-align:right'><a href='/status' style='font-size:14px'>View raw JSON</a></p>"
        "<div id='status-content' style='font-family:monospace;white-space:pre-wrap;background:#f8f8f8;padding:15px;border-radius:4px;overflow-x:auto'>"
        "Loading status..."
        "</div>"
        "</div>"
        "<script>"
        "fetch('/status').then(r=>r.json()).then(data=>{"
        "const fmt=(obj,indent=0)=>{"
        "const pad=' '.repeat(indent);"
        "let html='';"
        "for(const[k,v]of Object.entries(obj)){"
        "if(v===null){html+=pad+k+': null\\n';}"
        "else if(Array.isArray(v)){"
        "html+=pad+k+': [\\n';"
        "v.forEach(item=>{"
        "if(typeof item==='object'){html+=fmt(item,indent+2)+',\\n';}"
        "else{html+=pad+'  '+JSON.stringify(item)+',\\n';}"
        "});"
        "html+=pad+']\\n';"
        "}else if(typeof v==='object'){"
        "html+=pad+k+': {\\n'+fmt(v,indent+2)+pad+'}\\n';"
        "}else if(typeof v==='string'){"
        "html+=pad+k+': \"'+v+'\"\\n';"
        "}else{"
        "html+=pad+k+': '+v+'\\n';"
        "}"
        "}"
        "return html;"
        "};"
        "document.getElementById('status-content').textContent=fmt(data);"
        "}).catch(e=>{"
        "document.getElementById('status-content').innerHTML='<span style=\"color:red\">Error loading status: '+e+'</span>';"
        "});"
        "</script>";

    char page_title[] = "Pool Controller Status";
    char *header = get_page_header(page_title);
    char *nav = get_page_nav('s');
    char *footer = get_page_footer();

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nav, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, status_view_page, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, footer, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);

    free(footer);
    free(nav);
    free(header);
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
        "<div class='container'>"
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
        "</script></div>";


    char page_title[] = "MQTT Configuration";
    char *header = get_page_header(page_title);
    char *nav = get_page_nav('/');
    char *footer = get_page_footer();
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nav, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_start, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_mid, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_end, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, footer, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    free(footer);
    free(nav);
    free(header);

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
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Send success response
    const char *resp = "{\"success\":true,\"message\":\"MQTT config saved! Device will restart...\"}";
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    // Restart device to apply new MQTT config
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// ======================================================
// OTA Update Handlers
// ======================================================

static esp_err_t update_get_handler(httpd_req_t *req)
{
    // Get current firmware info
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    char partition_info[256];
    snprintf(partition_info, sizeof(partition_info),
             "Current: %s (%s)<br>Update will write to: %s",
             running->label, app_desc->version,
             update_partition ? update_partition->label : "unknown");

    const char *html_start =
        "<div class='container'>"
        "<h1>Firmware Update</h1>"
        "<div style='background:#e7f3ff;padding:15px;border-radius:4px;margin-bottom:20px'>"
        "<strong>Current Version:</strong> ";

    const char *html_mid =
        "</div>"
        "<div style='background:#fff3cd;padding:15px;border-radius:4px;margin-bottom:20px'>"
        "<strong>⚠️ Warning:</strong> Do not power off the device during update!"
        "</div>"
        "<form id='updateForm' enctype='multipart/form-data'>"
        "<label>Select Firmware File (.bin):</label>"
        "<input type='file' id='firmware' name='firmware' accept='.bin' required>"
        "<button type='submit'>Upload and Update</button>"
        "<div id='status' class='status'></div>"
        "<div id='progress' style='display:none;margin-top:20px'>"
        "<div style='background:#e0e0e0;border-radius:4px;overflow:hidden'>"
        "<div id='progressBar' style='background:#4CAF50;height:30px;width:0%;transition:width 0.3s'></div>"
        "</div>"
        "<div id='progressText' style='text-align:center;margin-top:10px'></div>"
        "</div>"
        "</form></div>"
        "<script>"
        "document.getElementById('updateForm').addEventListener('submit',function(e){"
        "e.preventDefault();"
        "const file=document.getElementById('firmware').files[0];"
        "if(!file){alert('Please select a file');return;}"
        "const status=document.getElementById('status');"
        "const progress=document.getElementById('progress');"
        "const progressBar=document.getElementById('progressBar');"
        "const progressText=document.getElementById('progressText');"
        "status.textContent='Uploading firmware ('+Math.round(file.size/1024)+'KB)...';status.className='status';status.style.display='block';"
        "progress.style.display='block';"
        "const xhr=new XMLHttpRequest();"
        "xhr.upload.addEventListener('progress',function(e){"
        "if(e.lengthComputable){"
        "const percent=Math.round((e.loaded/e.total)*100);"
        "progressBar.style.width=percent+'%';"
        "progressText.textContent=percent+'% ('+Math.round(e.loaded/1024)+'KB / '+Math.round(e.total/1024)+'KB)';"
        "}});"
        "xhr.addEventListener('load',function(){"
        "if(xhr.status===200){"
        "const resp=JSON.parse(xhr.responseText);"
        "if(resp.success){"
        "status.className='status success';"
        "let countdown=15;"
        "status.textContent='Update successful! Rebooting... reload in '+countdown+' seconds';"
        "const timer=setInterval(function(){"
        "countdown--;"
        "if(countdown>0){"
        "status.textContent='Update successful! Rebooting... reload in '+countdown+' seconds';"
        "}else{"
        "clearInterval(timer);"
        "status.textContent='Update successful! Rebooting... reloading now...';"
        "window.location.reload();"
        "}},1000);"
        "}else{"
        "status.textContent='Error: '+resp.message;status.className='status error';"
        "}}else{"
        "status.textContent='Upload failed (HTTP '+xhr.status+')';status.className='status error';"
        "}});"
        "xhr.addEventListener('error',function(){"
        "status.textContent='Network error during upload';status.className='status error';});"
        "xhr.addEventListener('timeout',function(){"
        "status.textContent='Upload timeout - try again';status.className='status error';});"
        "xhr.open('POST','/update',true);"
        "xhr.timeout=120000;"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.send(file);"
        "});"
        "</script>";

    char page_title[] = "Firmware Update";
    char *header = get_page_header(page_title);
    char *nav = get_page_nav('/');
    char *footer = get_page_footer();
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nav, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_start, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, partition_info, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_mid, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, footer, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    free(footer);
    free(nav);
    free(header);
    return ESP_OK;
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "OTA Update started - Content-Length: %d bytes", req->content_len);
    ESP_LOGI(TAG, "Running partition: %s at offset 0x%lx", running->label, running->address);

    // Log content type for debugging
    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        ESP_LOGI(TAG, "Content-Type: %s", content_type);
    }

    if (configured != running) {
        ESP_LOGW(TAG, "Configured boot partition != running partition");
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        const char *resp = "{\"success\":false,\"message\":\"No OTA partition configured\"}";
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%lx", update_partition->label, update_partition->address);

    // Start OTA update
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        const char *resp = "{\"success\":false,\"message\":\"OTA begin failed\"}";
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Read and write firmware data
    const size_t buf_size = 4096;  // 4KB buffer for better performance
    char *buf = malloc(buf_size);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        const char *resp = "{\"success\":false,\"message\":\"Memory allocation failed\"}";
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int received = 0;
    ESP_LOGI(TAG, "Firmware size: %d bytes", remaining);

    while (remaining > 0) {
        size_t recv_size = (remaining < buf_size) ? remaining : buf_size;
        int recv_len = httpd_req_recv(req, buf, recv_size);
        if (recv_len < 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Firmware receive failed");
            free(buf);
            esp_ota_abort(ota_handle);
            const char *resp = "{\"success\":false,\"message\":\"Receive failed\"}";
            httpd_resp_set_type(req, "application/json; charset=UTF-8");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }

        if (recv_len > 0) {
            err = esp_ota_write(ota_handle, (const void *)buf, recv_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                free(buf);
                esp_ota_abort(ota_handle);
                const char *resp = "{\"success\":false,\"message\":\"OTA write failed\"}";
                httpd_resp_set_type(req, "application/json; charset=UTF-8");
                httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
                return ESP_FAIL;
            }
            received += recv_len;
            remaining -= recv_len;

            // Log progress
            if (received % (100 * 1024) == 0 || remaining == 0) {
                ESP_LOGI(TAG, "Written %d/%d bytes (%.1f%%)", received, req->content_len,
                         100.0 * received / req->content_len);
            }
        }
    }

    free(buf);

    // Finish OTA update
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            const char *resp = "{\"success\":false,\"message\":\"Image validation failed\"}";
            httpd_resp_set_type(req, "application/json; charset=UTF-8");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            const char *resp = "{\"success\":false,\"message\":\"OTA end failed\"}";
            httpd_resp_set_type(req, "application/json; charset=UTF-8");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        }
        return ESP_FAIL;
    }

    // Set boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        const char *resp = "{\"success\":false,\"message\":\"Failed to set boot partition\"}";
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful!");

    const char *resp = "{\"success\":true,\"message\":\"Update successful! Rebooting...\"}";
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    // Restart after a delay
    vTaskDelay(pdMS_TO_TICKS(2000));
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

static const httpd_uri_t status_view_uri = {
    .uri = "/status_view",
    .method = HTTP_GET,
    .handler = status_view_get_handler
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

static const httpd_uri_t update_get_uri = {
    .uri = "/update",
    .method = HTTP_GET,
    .handler = update_get_handler
};

static const httpd_uri_t update_post_uri = {
    .uri = "/update",
    .method = HTTP_POST,
    .handler = update_post_handler
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
    httpd_register_uri_handler(server, &status_view_uri);
    httpd_register_uri_handler(server, &mqtt_config_get_uri);
    httpd_register_uri_handler(server, &mqtt_config_post_uri);
    httpd_register_uri_handler(server, &update_get_uri);
    httpd_register_uri_handler(server, &update_post_uri);
    ESP_LOGI(TAG, "Web/HTTP handlers registered");
    return ESP_OK;
}

