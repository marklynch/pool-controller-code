#include "web_handlers.h"
#include "config.h"
#include "wifi_provisioning.h"
#include "pool_state.h"
#include "mqtt_poolclient.h"
#include "message_decoder.h"
#include "tcp_bridge.h"
#include "device_serial.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include <string.h>
#include <inttypes.h>
#include "cJSON.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "WEB_HANDLERS";

// Forward declaration - defined in main.c
const char* get_gateway_comms_status_text(uint16_t code);

// ======================================================
// Common HTML Page Parts (Header and footer)
// ======================================================

// Dynamic functions for page header, navigation, and footer
char *get_page_header(const char *title) {
    const char *fmt = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' type='image/png' href='/static/favicon.png'>"
        "<link rel='icon' type='image/svg+xml' href='/static/favicon.ico'>"
        "<link rel='apple-touch-icon' href='/static/favicon.png'>"
        "<link rel='stylesheet' href='/static/oat.min.css'>"
        "<script src='/static/oat.min.js' defer></script>"
        "<title>%s</title></head><body><div data-sidebar-layout>";
    int n = snprintf(NULL, 0, fmt, title);
    if (n < 0) return NULL;

    char *header = malloc((size_t)n + 1);
    if (!header) return NULL;

    snprintf(header, (size_t)n + 1, fmt, title);
    return header;
}

char *get_page_nav(const char page) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *fmt =
        "<nav data-topnav>"
        "<button data-sidebar-toggle aria-label='Toggle menu' class='outline'>☰</button>"
        "<img src='/static/favicon.ico' alt='Logo' style='height:1.6em;'>"
        "<span>Pool Controller</span>"
        "</nav>"
        "<aside data-sidebar>"
        "<nav><ul>"
        "<li><a href='/'" "%s" ">Home</a></li>"
        "<li><a href='/wifi'" "%s" ">WiFi Config</a></li>"
        "<li><a href='/mqtt_config'" "%s" ">MQTT Config</a></li>"
        "<li><a href='/status_view'" "%s" ">Status</a></li>"
        "<li><a href='/update'" "%s" ">Firmware Update</a></li>"
        "</ul></nav>"
        "<footer><small>%s</small></footer>"
        "</aside>"
        "<main class='p-4'>";
    const char *cur = " aria-current='page'";
    const char *none = "";
    int n = snprintf(NULL, 0, fmt,
        page == 'h' ? cur : none,
        page == 'w' ? cur : none,
        page == 'm' ? cur : none,
        page == 's' ? cur : none,
        page == 'u' ? cur : none,
        app_desc->version);
    if (n < 0) return NULL;

    char *nav = malloc((size_t)n + 1);
    if (!nav) return NULL;

    snprintf(nav, (size_t)n + 1, fmt,
        page == 'h' ? cur : none,
        page == 'w' ? cur : none,
        page == 'm' ? cur : none,
        page == 's' ? cur : none,
        page == 'u' ? cur : none,
        app_desc->version);
    return nav;
}

char *get_page_footer(void) {
    const char *str = "</main></div></body></html>";
    size_t len = strlen(str);
    char *footer = malloc(len + 1);
    if (!footer) return NULL;
    memcpy(footer, str, len + 1);
    return footer;
}

// ======================================================
// Home Page Handler
// ======================================================

static esp_err_t home_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    // WiFi AP info (SSID, RSSI)
    wifi_ap_record_t ap_info = {0};
    bool wifi_info_ok = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

    // MQTT status
    bool mqtt_connected = mqtt_client_is_connected();
    mqtt_config_t mqtt_config = {0};
    bool mqtt_enabled = mqtt_load_config(&mqtt_config) && mqtt_config.enabled;

    // Uptime
    uint32_t uptime_s = (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    char uptime_str[32];
    uint32_t d = uptime_s / 86400, h = (uptime_s % 86400) / 3600;
    uint32_t m = (uptime_s % 3600) / 60, s = uptime_s % 60;
    if (d > 0)      snprintf(uptime_str, sizeof(uptime_str), "%lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)m);
    else if (h > 0) snprintf(uptime_str, sizeof(uptime_str), "%luh %lum %lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    else            snprintf(uptime_str, sizeof(uptime_str), "%lum %lus", (unsigned long)m, (unsigned long)s);

    // Serial number
    char serial[DEVICE_SERIAL_LEN];
    device_get_serial(serial, sizeof(serial));

    // mDNS hostname
    const char *mdns_host = wifi_get_mdns_hostname();

    // System info table
    char sys_table[1024];
    snprintf(sys_table, sizeof(sys_table),
        "<h1>Pool Controller</h1>"
        "<h2>System</h2>"
        "<table><tbody id='sys-body'>"
        "<tr><th>Serial</th><td>%s</td></tr>"
        "<tr><th>Firmware</th><td>%s</td></tr>"
        "<tr><th>Project</th><td>%s</td></tr>"
        "<tr><th>Built</th><td>%s %s</td></tr>"
        "<tr><th>Uptime</th><td>%s</td></tr>"
        "<tr><th>IP Address</th><td>%s</td></tr>"
        "<tr><th>Hostname</th><td><a href='http://%s.local/'>%s.local</a></td></tr>",
        serial,
        app_desc->version, app_desc->project_name,
        app_desc->date, app_desc->time,
        uptime_str, wifi_get_device_ip(),
        mdns_host, mdns_host);

    // WiFi row
    char wifi_row[96];
    if (wifi_info_ok) {
        snprintf(wifi_row, sizeof(wifi_row),
            "<tr><th>WiFi</th><td>%s (%d dBm)</td></tr>",
            (char *)ap_info.ssid, ap_info.rssi);
    } else {
        snprintf(wifi_row, sizeof(wifi_row), "<tr><th>WiFi</th><td>Not connected</td></tr>");
    }

    // MQTT row
    char mqtt_row[256];
    if (!mqtt_enabled) {
        snprintf(mqtt_row, sizeof(mqtt_row), "<tr><th>MQTT</th><td>Disabled</td></tr></tbody></table>");
    } else if (mqtt_connected) {
        snprintf(mqtt_row, sizeof(mqtt_row),
            "<tr><th>MQTT</th><td>Connected (%s)</td></tr></tbody></table>", mqtt_config.broker);
    } else {
        snprintf(mqtt_row, sizeof(mqtt_row),
            "<tr><th>MQTT</th><td>Disconnected (%s)</td></tr></tbody></table>", mqtt_config.broker);
    }

    // Pool summary — loaded from /status via JS
    static const char pool_section[] =
        "<h2>Pool</h2>"
        "<p id='pool-loading' class='text-lighter'>Loading...</p>"
        "<table id='pool-table' hidden><tbody id='pool-body'></tbody></table>"
        "<p id='time-info' class='text-lighter mt-2' hidden></p>"
        "<div id='pool-error' role='alert' data-variant='danger' hidden></div>"
        "<script>"
        "fetch('/status').then(r=>r.json()).then(data=>{"
        "const rows=[];"
        "if(data.mode!==null)rows.push(['Mode',data.mode]);"
        "const t=data.temperature;"
        "if(t.current!==null)rows.push(['Temperature',t.current+'\u00b0'+(t.scale==='Fahrenheit'?'F':'C')]);"
        "if(data.heater.state!==null)rows.push(['Heater',data.heater.state]);"
        "data.channels.forEach(ch=>rows.push([ch.name||ch.type,ch.state]));"
        "data.lighting.forEach(lt=>{"
        "let s=lt.state;if(lt.active&&lt.color)s+=': '+lt.color;"
        "rows.push(['Lighting zone '+lt.zone,s]);});"
        "const c=data.chlorinator;"
        "if(c.ph_reading!==null)rows.push(['pH',c.ph_reading+' (setpoint '+c.ph_setpoint+')']);"
        "if(c.orp_reading!==null)rows.push(['ORP',c.orp_reading+'mV (setpoint '+c.orp_setpoint+'mV)']);"
        "if(data.timers&&data.timers.length>0){"
        "data.timers.forEach(t=>rows.push(['Timer '+t.num,t.start+' \u2013 '+t.stop+' ['+t.days+']']));}"
        "const mc=data.message_counts;"
        "if(mc){"
        "const tot=mc.decoded+mc.unknown;"
        "const pct=tot>0?(mc.decoded/tot*100).toFixed(1)+'%':'n/a';"
        "const mtr=document.createElement('tr');"
        "mtr.innerHTML='<th>Messages</th><td>'+mc.decoded+' decoded, '+mc.unknown+' unknown ('+pct+')</td>';"
        "document.getElementById('sys-body').appendChild(mtr);}"
        "const tb=document.getElementById('pool-body');"
        "rows.forEach(([k,v])=>{"
        "const tr=document.createElement('tr');"
        "tr.innerHTML='<th>'+k+'</th><td>'+v+'</td>';"
        "tb.appendChild(tr);});"
        "document.getElementById('pool-loading').hidden=true;"
        "document.getElementById('pool-table').removeAttribute('hidden');"
        "if(data.time_since_last_update){"
        "const ti=document.getElementById('time-info');"
        "ti.textContent='Last update: '+data.time_since_last_update;"
        "ti.removeAttribute('hidden');}"
        "}).catch(e=>{"
        "document.getElementById('pool-loading').hidden=true;"
        "const err=document.getElementById('pool-error');"
        "err.textContent='Failed to load pool status: '+e;"
        "err.removeAttribute('hidden');});"
        "</script>";

    char page_title[] = "Home";
    char *header = get_page_header(page_title);
    char *nav = get_page_nav('h');
    char *footer = get_page_footer();

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nav, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, sys_table, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, wifi_row, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, mqtt_row, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, pool_section, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, footer, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);

    free(footer);
    free(nav);
    free(header);
    return ESP_OK;
}

// ======================================================
// WiFi Config Page Handler
// ======================================================

// HTML page for WiFi provisioning
const char WIFI_PAGE[] =
    "<h1>WiFi Configuration</h1>"
    "<form id='wifiForm'>"
    "<div data-field>"
        "<label for='ssid'>WiFi Network</label>"
        "<select id='ssid' name='ssid' required>"
            "<option value=''>Scanning...</option>"
        "</select>"
    "</div>"
    "<div data-field>"
        "<label for='password'>Password</label>"
        "<input type='password' id='password' name='password' placeholder='Enter network password' required>"
    "</div>"
    "<button type='submit'>Connect</button>"
    "<div id='status' role='alert' hidden class='mt-4'></div>"
    "</form>"
    "<script>"
    "function showStatus(msg,isError){"
        "const s=document.getElementById('status');"
        "s.textContent=msg;"
        "s.setAttribute('data-variant',isError?'danger':'success');"
        "s.removeAttribute('hidden');}"
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
        "if(data.success){showStatus('Connected! Device will restart.',false);}"
        "else{showStatus('Failed: '+data.message,true);}}).catch(e=>{"
        "showStatus('Error: '+e,true);});return false;};"
    "scanWiFi();"
    "</script>";

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    char page_title[] = "WiFi Configuration";
    char *header = get_page_header(page_title);
    char *nav = get_page_nav('w');
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
    // Load current WiFi SSID from the driver to mark it in scan results.
    char current_ssid[33] = {0};
    wifi_config_t wifi_cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
        strncpy(current_ssid, (char *)wifi_cfg.sta.ssid, sizeof(current_ssid) - 1);
    }
    ESP_LOGI(TAG, "Current SSID: '%s'", current_ssid);

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = WIFI_SCAN_TIME_MIN_MS,
        .scan_time.active.max = WIFI_SCAN_TIME_MAX_MS,
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

    // Build JSON response from unique list using cJSON to correctly escape SSIDs
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        free(ap_list);
        free(unique_aps);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    for (int i = 0; i < unique_count && i < WIFI_SCAN_MAX_RESULTS; i++) {
        bool is_current = (strcmp(unique_aps[i].ssid, current_ssid) == 0);
        ESP_LOGI(TAG, "Comparing '%s' with '%s': %s", unique_aps[i].ssid, current_ssid, is_current ? "MATCH" : "no match");
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "ssid", unique_aps[i].ssid);
        cJSON_AddNumberToObject(entry, "rssi", unique_aps[i].rssi);
        cJSON_AddBoolToObject(entry, "current", is_current);
        cJSON_AddItemToArray(root, entry);
    }

    char *json_resp = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_resp) {
        free(ap_list);
        free(unique_aps);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON serialisation failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, json_resp, strlen(json_resp));

    cJSON_free(json_resp);
    free(ap_list);
    free(unique_aps);
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
    char content[HTTP_PROVISION_BUFFER_SIZE];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    char ssid[33] = {0};
    char password[64] = {0};

    cJSON *json = cJSON_Parse(content);
    if (json) {
        cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
        cJSON *pass_item = cJSON_GetObjectItem(json, "password");
        if (cJSON_IsString(ssid_item) && ssid_item->valuestring) {
            strncpy(ssid, ssid_item->valuestring, sizeof(ssid) - 1);
        }
        if (cJSON_IsString(pass_item) && pass_item->valuestring) {
            strncpy(password, pass_item->valuestring, sizeof(password) - 1);
        }
        cJSON_Delete(json);
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
    vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
    esp_restart();

    return ESP_OK;
}

// ======================================================
// Pool Status Handler
// ======================================================

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to acquire state mutex");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();

    // Firmware
    char compile_time[48];
    snprintf(compile_time, sizeof(compile_time), "%s %s", app_desc->date, app_desc->time);
    cJSON *firmware = cJSON_CreateObject();
    cJSON_AddStringToObject(firmware, "version",      app_desc->version);
    cJSON_AddStringToObject(firmware, "project",      app_desc->project_name);
    cJSON_AddStringToObject(firmware, "compile_time", compile_time);
    cJSON_AddStringToObject(firmware, "idf_version",  app_desc->idf_ver);
    cJSON_AddItemToObject(root, "firmware", firmware);

    // Message decode counters
    cJSON *msg_counts = cJSON_CreateObject();
    cJSON_AddNumberToObject(msg_counts, "decoded", tcp_bridge_get_decoded_count());
    cJSON_AddNumberToObject(msg_counts, "unknown", tcp_bridge_get_unknown_count());
    cJSON_AddItemToObject(root, "message_counts", msg_counts);

    // Temperature
    cJSON *temperature = cJSON_CreateObject();
    if (s_pool_state.temp_valid) {
        cJSON_AddNumberToObject(temperature, "current", s_pool_state.current_temp);
    } else {
        cJSON_AddNullToObject(temperature, "current");
    }
    cJSON_AddNumberToObject(temperature, "pool_setpoint",   s_pool_state.pool_setpoint);
    cJSON_AddNumberToObject(temperature, "spa_setpoint",    s_pool_state.spa_setpoint);
    cJSON_AddNumberToObject(temperature, "pool_setpoint_f", s_pool_state.pool_setpoint_f);
    cJSON_AddNumberToObject(temperature, "spa_setpoint_f",  s_pool_state.spa_setpoint_f);
    cJSON_AddStringToObject(temperature, "scale", s_pool_state.temp_scale_fahrenheit ? "Fahrenheit" : "Celsius");
    cJSON_AddItemToObject(root, "temperature", temperature);

    // Heater
    cJSON *heater = cJSON_CreateObject();
    if (s_pool_state.heater_valid) {
        cJSON_AddStringToObject(heater, "state", s_pool_state.heater_on ? "On" : "Off");
    } else {
        cJSON_AddNullToObject(heater, "state");
    }
    cJSON_AddItemToObject(root, "heater", heater);

    // Mode
    if (s_pool_state.mode_valid) {
        const char *mode_str = (s_pool_state.mode == 0) ? "Spa" :
                               (s_pool_state.mode == 1) ? "Pool" : "Unknown";
        cJSON_AddStringToObject(root, "mode", mode_str);
    } else {
        cJSON_AddNullToObject(root, "mode");
    }

    // Channels (excludes heater and light-zone channels — those appear in their own sections)
    cJSON *channels = cJSON_CreateArray();
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!s_pool_state.channels[i].configured ||
            s_pool_state.channels[i].type == CHANNEL_TYPE_HEATER ||
            s_pool_state.channels[i].type == CHANNEL_TYPE_LIGHT_ZONE) {
            continue;
        }
        const char *type_name  = get_channel_type_name(s_pool_state.channels[i].type);
        const char *state_name = (s_pool_state.channels[i].state < CHANNEL_STATE_COUNT) ?
                                 CHANNEL_STATE_NAMES[s_pool_state.channels[i].state] : "Unknown";
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddNumberToObject(ch, "id",    s_pool_state.channels[i].id);
        cJSON_AddStringToObject(ch, "name",  s_pool_state.channels[i].name[0] ? s_pool_state.channels[i].name : type_name);
        cJSON_AddStringToObject(ch, "type",  type_name);
        cJSON_AddStringToObject(ch, "state", state_name);
        cJSON_AddItemToArray(channels, ch);
    }
    cJSON_AddItemToObject(root, "channels", channels);

    // Lighting
    cJSON *lighting = cJSON_CreateArray();
    for (int i = 0; i < MAX_LIGHT_ZONES; i++) {
        if (!s_pool_state.lighting[i].configured) continue;
        const char *state_name = (s_pool_state.lighting[i].state < LIGHTING_STATE_COUNT) ?
                                 LIGHTING_STATE_NAMES[s_pool_state.lighting[i].state] : "Unknown";
        const char *color_name = (s_pool_state.lighting[i].color < LIGHTING_COLOR_COUNT) ?
                                 LIGHTING_COLOR_NAMES[s_pool_state.lighting[i].color] : "Unknown";
        cJSON *zone = cJSON_CreateObject();
        cJSON_AddNumberToObject(zone, "zone", s_pool_state.lighting[i].zone);
        if (s_pool_state.lighting[i].name_valid &&
            s_pool_state.lighting[i].name_id < LIGHT_ZONE_NAME_COUNT) {
            cJSON_AddStringToObject(zone, "name", LIGHT_ZONE_NAME_TABLE[s_pool_state.lighting[i].name_id]);
        } else {
            cJSON_AddNullToObject(zone, "name");
        }
        if (s_pool_state.lighting[i].multicolor_valid) {
            cJSON_AddBoolToObject(zone, "multicolor", s_pool_state.lighting[i].multicolor);
        } else {
            cJSON_AddNullToObject(zone, "multicolor");
        }
        cJSON_AddStringToObject(zone, "state",  state_name);
        cJSON_AddStringToObject(zone, "color",  color_name);
        cJSON_AddBoolToObject(zone,   "active", s_pool_state.lighting[i].active);
        cJSON_AddItemToArray(lighting, zone);
    }
    cJSON_AddItemToObject(root, "lighting", lighting);

    // Valves
    cJSON *valves = cJSON_CreateArray();
    for (int i = 0; i < s_pool_state.num_valve_slots && i < MAX_VALVE_SLOTS; i++) {
        if (!s_pool_state.valves[i].configured) continue;

        // Look up label from register_labels (reg_id 0xD0 = valve 1, 0xD1 = valve 2, ...)
        uint8_t reg_id = 0xD0 + i;
        const char *label = NULL;
        for (int j = 0; j < MAX_REGISTER_LABELS; j++) {
            if (s_pool_state.register_labels[j].valid &&
                s_pool_state.register_labels[j].reg_id == reg_id) {
                label = s_pool_state.register_labels[j].label;
                break;
            }
        }
        char fallback[16];
        if (!label) {
            snprintf(fallback, sizeof(fallback), "Valve %d", i + 1);
            label = fallback;
        }

        const char *state_name = (s_pool_state.valves[i].state < CHANNEL_STATE_COUNT) ?
                                 CHANNEL_STATE_NAMES[s_pool_state.valves[i].state] : "Unknown";
        cJSON *valve = cJSON_CreateObject();
        cJSON_AddNumberToObject(valve, "id",     i + 1);
        cJSON_AddStringToObject(valve, "name",   label);
        cJSON_AddStringToObject(valve, "state",  state_name);
        cJSON_AddBoolToObject(valve,   "active", s_pool_state.valves[i].active);
        cJSON_AddItemToArray(valves, valve);
    }
    cJSON_AddItemToObject(root, "valves", valves);

    // Chlorinator
    cJSON *chlorinator = cJSON_CreateObject();
    if (s_pool_state.ph_valid) {
        cJSON_AddNumberToObject(chlorinator, "ph_setpoint", s_pool_state.ph_setpoint / 10.0);
        cJSON_AddNumberToObject(chlorinator, "ph_reading",  s_pool_state.ph_reading  / 10.0);
    } else {
        cJSON_AddNullToObject(chlorinator, "ph_setpoint");
        cJSON_AddNullToObject(chlorinator, "ph_reading");
    }
    if (s_pool_state.orp_valid) {
        cJSON_AddNumberToObject(chlorinator, "orp_setpoint", s_pool_state.orp_setpoint);
        cJSON_AddNumberToObject(chlorinator, "orp_reading",  s_pool_state.orp_reading);
    } else {
        cJSON_AddNullToObject(chlorinator, "orp_setpoint");
        cJSON_AddNullToObject(chlorinator, "orp_reading");
    }
    cJSON_AddItemToObject(root, "chlorinator", chlorinator);

    // Internet Gateway
    cJSON *gateway = cJSON_CreateObject();
    if (s_pool_state.serial_number_valid) {
        cJSON_AddNumberToObject(gateway, "serial_number", (double)s_pool_state.serial_number);
    } else {
        cJSON_AddNullToObject(gateway, "serial_number");
    }
    if (s_pool_state.gateway_version_valid) {
        char fw_ver[16];
        snprintf(fw_ver, sizeof(fw_ver), "%d.%d",
                 s_pool_state.gateway_version_major, s_pool_state.gateway_version_minor);
        cJSON_AddStringToObject(gateway, "firmware_version", fw_ver);
    } else {
        cJSON_AddNullToObject(gateway, "firmware_version");
    }
    if (s_pool_state.gateway_ip_valid) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
                 s_pool_state.gateway_ip[0], s_pool_state.gateway_ip[1],
                 s_pool_state.gateway_ip[2], s_pool_state.gateway_ip[3]);
        cJSON_AddStringToObject(gateway, "ip",           ip_str);
        cJSON_AddNumberToObject(gateway, "signal_level", s_pool_state.gateway_signal_level);
    } else {
        cJSON_AddNullToObject(gateway, "ip");
        cJSON_AddNullToObject(gateway, "signal_level");
    }
    if (s_pool_state.gateway_comms_status_valid) {
        cJSON_AddNumberToObject(gateway, "comms_status", s_pool_state.gateway_comms_status);
        cJSON_AddStringToObject(gateway, "comms_status_text",
                                get_gateway_comms_status_text(s_pool_state.gateway_comms_status));
    } else {
        cJSON_AddNullToObject(gateway, "comms_status");
        cJSON_AddNullToObject(gateway, "comms_status_text");
    }
    cJSON_AddItemToObject(root, "internet_gateway", gateway);

    // Touchscreen
    cJSON *touchscreen = cJSON_CreateObject();
    if (s_pool_state.touchscreen_version_valid) {
        char ts_ver[16];
        snprintf(ts_ver, sizeof(ts_ver), "%d.%d",
                 s_pool_state.touchscreen_version_major, s_pool_state.touchscreen_version_minor);
        cJSON_AddStringToObject(touchscreen, "version", ts_ver);
    } else {
        cJSON_AddNullToObject(touchscreen, "version");
    }
    cJSON_AddNumberToObject(touchscreen, "num_channels", s_pool_state.num_channels);
    cJSON_AddItemToObject(root, "touchscreen", touchscreen);

    // Timers (only non-empty entries)
    cJSON *timers = cJSON_CreateArray();
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_state_t *t = &s_pool_state.timers[i];
        if (!t->valid) continue;
        if (t->days == 0 && t->start_hour == 0 && t->start_minute == 0 &&
            t->stop_hour == 0 && t->stop_minute == 0) continue;

        char start_str[9], stop_str[9];
        snprintf(start_str, sizeof(start_str), "%02d:%02d", t->start_hour, t->start_minute);
        snprintf(stop_str,  sizeof(stop_str),  "%02d:%02d", t->stop_hour,  t->stop_minute);

        char days_str[8];
        const char day_chars[] = "MTWTFSS";
        for (int d = 0; d < 7; d++) {
            days_str[d] = (t->days & (1 << d)) ? day_chars[d] : '-';
        }
        days_str[7] = '\0';

        cJSON *timer = cJSON_CreateObject();
        cJSON_AddNumberToObject(timer, "num",   t->timer_num);
        cJSON_AddStringToObject(timer, "start", start_str);
        cJSON_AddStringToObject(timer, "stop",  stop_str);
        cJSON_AddStringToObject(timer, "days",  days_str);
        cJSON_AddItemToArray(timers, timer);
    }
    cJSON_AddItemToObject(root, "timers", timers);

    // Timestamps
    uint64_t current_tick_ms = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    cJSON_AddNumberToObject(root, "last_update_ms", (double)s_pool_state.last_update_ms);
    cJSON_AddNumberToObject(root, "current_ms",     (double)current_tick_ms);

    time_t now = time(NULL);
    char time_str[32];
    if (now < 1000000000) {
        snprintf(time_str, sizeof(time_str), "NTP not synced");
    } else {
        struct tm *tm_info = gmtime(&now);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", tm_info);
    }
    cJSON_AddStringToObject(root, "current_time", time_str);

    char age_str[32];
    if (s_pool_state.last_update_ms == 0) {
        snprintf(age_str, sizeof(age_str), "Never");
    } else {
        uint32_t elapsed_sec = (uint32_t)((current_tick_ms - s_pool_state.last_update_ms) / 1000);
        if (elapsed_sec < 60) {
            snprintf(age_str, sizeof(age_str), "%lus ago", (unsigned long)elapsed_sec);
        } else if (elapsed_sec < 3600) {
            snprintf(age_str, sizeof(age_str), "%lum %lus ago",
                     (unsigned long)(elapsed_sec / 60), (unsigned long)(elapsed_sec % 60));
        } else {
            snprintf(age_str, sizeof(age_str), "%luh %lum ago",
                     (unsigned long)(elapsed_sec / 3600), (unsigned long)((elapsed_sec % 3600) / 60));
        }
    }
    cJSON_AddStringToObject(root, "time_since_last_update", age_str);

    xSemaphoreGive(s_pool_state_mutex);

    char *json_resp = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON serialisation failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, json_resp, strlen(json_resp));

    cJSON_free(json_resp);
    return ESP_OK;
}

// ======================================================
// Status View Handler (HTML formatted view)
// ======================================================

static esp_err_t status_view_get_handler(httpd_req_t *req)
{
    const char *status_view_page =
        "<h1>Pool Controller Status</h1>"
        "<p class='text-right'><small><a href='/status'>View raw JSON</a></small></p>"
        "<div id='status-error' role='alert' data-variant='danger' hidden></div>"
        "<pre><code id='status-content'>Loading status...</code></pre>"
        "<p id='time-info' class='text-lighter mt-4' hidden></p>"
        "<script>"
        "fetch('/status').then(r=>r.json()).then(data=>{"
        "const fmt=(obj,indent=0)=>{"
        "const pad=' '.repeat(indent);"
        "let out='';"
        "for(const[k,v]of Object.entries(obj)){"
        "if(v===null){out+=pad+k+': null\\n';}"
        "else if(Array.isArray(v)){"
        "out+=pad+k+': [\\n';"
        "v.forEach(item=>{"
        "if(typeof item==='object'){out+=fmt(item,indent+2)+',\\n';}"
        "else{out+=pad+'  '+JSON.stringify(item)+',\\n';}"
        "});"
        "out+=pad+']\\n';"
        "}else if(typeof v==='object'){"
        "out+=pad+k+': {\\n'+fmt(v,indent+2)+pad+'}\\n';"
        "}else if(typeof v==='string'){"
        "out+=pad+k+': \"'+v+'\"\\n';"
        "}else{"
        "out+=pad+k+': '+v+'\\n';"
        "}"
        "}"
        "return out;"
        "};"
        "document.getElementById('status-content').textContent=fmt(data);"
        "const ti=document.getElementById('time-info');"
        "let localTime='NTP not synced';"
        "if(data.current_time&&data.current_time!=='NTP not synced'){"
        "const d=new Date(data.current_time.replace(' UTC','Z').replace(' ','T'));"
        "const tz=Intl.DateTimeFormat().resolvedOptions().timeZone;"
        "localTime=d.toLocaleString()+' ('+tz+')';}"
        "let ageStr='Never';"
        "if(data.last_update_ms){"
        "const s=Math.floor((data.current_ms-data.last_update_ms)/1000);"
        "if(s<60)ageStr=s+'s ago';"
        "else if(s<3600)ageStr=Math.floor(s/60)+'m '+(s%60)+'s ago';"
        "else ageStr=Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m ago';}"
        "ti.innerHTML='Time: <strong>'+localTime+'</strong> &nbsp;|&nbsp; Last update: <strong>'+ageStr+'</strong>';"
        "ti.removeAttribute('hidden');"
        "}).catch(e=>{"
        "const err=document.getElementById('status-error');"
        "err.textContent='Error loading status: '+e;"
        "err.removeAttribute('hidden');"
        "document.getElementById('status-content').textContent='';"
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
    mqtt_config_t config = {0};
    mqtt_load_config(&config);

    int display_port = config.port > 0 ? config.port : MQTT_DEFAULT_PORT;

    const char *html_start =
        "<h1>MQTT Configuration</h1>"
        "<form id='mqttForm'>";

    char html_fields[1536];
    snprintf(html_fields, sizeof(html_fields),
        "<div data-field>"
            "<label><input type='checkbox' id='enabled' role='switch' name='enabled'%s> Enable MQTT</label>"
        "</div>"
        "<div data-field>"
            "<label for='broker'>Broker Host/IP</label>"
            "<input type='text' id='broker' name='broker' value='%s' placeholder='mqtt.example.com'>"
        "</div>"
        "<div data-field>"
            "<label for='port'>Port</label>"
            "<input type='number' id='port' name='port' value='%d' min='1' max='65535'>"
        "</div>"
        "<div data-field>"
            "<label for='username'>Username <small>(optional)</small></label>"
            "<input type='text' id='username' name='username' value='%s' placeholder='Leave empty if not required'>"
        "</div>"
        "<div data-field>"
            "<label for='password'>Password <small>(optional)</small></label>"
            "<input type='password' id='password' name='password' placeholder='%s'>"
        "</div>",
        config.enabled ? " checked" : "",
        config.broker, display_port, config.username,
        strlen(config.password) > 0 ? "Leave empty to keep current password" : "Leave empty if not required");

    const char *html_end =
        "<button type='submit'>Save Configuration</button>"
        "<div id='status' role='alert' hidden class='mt-4'></div>"
        "</form>"
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
        "s.setAttribute('data-variant',d.success?'success':'danger');"
        "s.removeAttribute('hidden');"
        "if(d.success)setTimeout(()=>window.location.reload(),2000);"
        "}).catch(()=>{"
        "const s=document.getElementById('status');"
        "s.textContent='Failed to save configuration';"
        "s.setAttribute('data-variant','danger');"
        "s.removeAttribute('hidden');});"
        "});"
        "</script>";

    char page_title[] = "MQTT Configuration";
    char *header = get_page_header(page_title);
    char *nav = get_page_nav('m');
    char *footer = get_page_footer();
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nav, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_start, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_fields, HTTPD_RESP_USE_STRLEN);
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
    char content[HTTP_MQTT_CONFIG_BUFFER_SIZE];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Load existing config first (to preserve password if not changed)
    mqtt_config_t config = {0};
    mqtt_load_config(&config);

    // If no existing port, default to MQTT_DEFAULT_PORT
    if (config.port == 0) {
        config.port = MQTT_DEFAULT_PORT;
    }

    cJSON *json = cJSON_Parse(content);
    if (json) {
        cJSON *enabled_item  = cJSON_GetObjectItem(json, "enabled");
        cJSON *broker_item   = cJSON_GetObjectItem(json, "broker");
        cJSON *port_item     = cJSON_GetObjectItem(json, "port");
        cJSON *username_item = cJSON_GetObjectItem(json, "username");
        cJSON *password_item = cJSON_GetObjectItem(json, "password");

        if (cJSON_IsBool(enabled_item)) {
            config.enabled = cJSON_IsTrue(enabled_item);
        }
        if (cJSON_IsString(broker_item) && broker_item->valuestring) {
            strncpy(config.broker, broker_item->valuestring, sizeof(config.broker) - 1);
        }
        if (cJSON_IsNumber(port_item)) {
            config.port = (uint16_t)port_item->valueint;
        }
        if (cJSON_IsString(username_item) && username_item->valuestring) {
            strncpy(config.username, username_item->valuestring, sizeof(config.username) - 1);
        }
        // Only update password if a non-empty value was submitted; otherwise keep existing
        if (cJSON_IsString(password_item) && password_item->valuestring &&
            password_item->valuestring[0] != '\0') {
            strncpy(config.password, password_item->valuestring, sizeof(config.password) - 1);
        }
        cJSON_Delete(json);
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
    vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
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
        "<h1>Firmware Update</h1>"
        "<div role='alert'>"
        "<strong>Current Version:</strong> ";

    const char *html_mid =
        "</div>"
        "<div role='alert' data-variant='warning' class='mt-4'>"
        "<strong>Warning:</strong> Do not power off the device during update!"
        "</div>"
        "<form id='updateForm' class='mt-4'>"
        "<div data-field>"
            "<label for='firmware'>Firmware File (.bin)</label>"
            "<input type='file' id='firmware' name='firmware' accept='.bin' required>"
        "</div>"
        "<button type='submit'>Upload and Update</button>"
        "<div id='status' role='alert' hidden class='mt-4'></div>"
        "<progress id='progressBar' value='0' max='100' hidden class='mt-4'></progress>"
        "<p id='progressText' hidden class='text-center mt-2'></p>"
        "</form>"
        "<script>"
        "document.getElementById('updateForm').addEventListener('submit',function(e){"
        "e.preventDefault();"
        "const file=document.getElementById('firmware').files[0];"
        "if(!file){alert('Please select a file');return;}"
        "const status=document.getElementById('status');"
        "const progressBar=document.getElementById('progressBar');"
        "const progressText=document.getElementById('progressText');"
        "status.textContent='Uploading firmware ('+Math.round(file.size/1024)+'KB)...';"
        "status.removeAttribute('data-variant');"
        "status.removeAttribute('hidden');"
        "progressBar.value=0;"
        "progressBar.removeAttribute('hidden');"
        "progressText.removeAttribute('hidden');"
        "const xhr=new XMLHttpRequest();"
        "xhr.upload.addEventListener('progress',function(e){"
        "if(e.lengthComputable){"
        "const percent=Math.round((e.loaded/e.total)*100);"
        "progressBar.value=percent;"
        "progressText.textContent=percent+'% ('+Math.round(e.loaded/1024)+'KB / '+Math.round(e.total/1024)+'KB)';"
        "}});"
        "xhr.addEventListener('load',function(){"
        "if(xhr.status===200){"
        "const resp=JSON.parse(xhr.responseText);"
        "if(resp.success){"
        "progressBar.setAttribute('hidden','');progressText.setAttribute('hidden','');"
        "status.setAttribute('data-variant','success');"
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
        "status.textContent='Error: '+resp.message;"
        "status.setAttribute('data-variant','danger');"
        "}}else{"
        "status.textContent='Upload failed (HTTP '+xhr.status+')';"
        "status.setAttribute('data-variant','danger');"
        "}});"
        "xhr.addEventListener('error',function(){"
        "status.textContent='Network error during upload';"
        "status.setAttribute('data-variant','danger');});"
        "xhr.addEventListener('timeout',function(){"
        "status.textContent='Upload timeout - try again';"
        "status.setAttribute('data-variant','danger');});"
        "xhr.open('POST','/update',true);"
        "xhr.timeout=120000;"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.send(file);"
        "});"
        "</script>";

    char page_title[] = "Firmware Update";
    char *header = get_page_header(page_title);
    char *nav = get_page_nav('u');
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
    const size_t buf_size = HTTP_OTA_BUFFER_SIZE;
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
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();

    return ESP_OK;
}

// ======================================================
// Test Decode Handler
// ======================================================

// Forward declaration - defined in message_decoder.c
extern bool decode_message(const uint8_t *data, int len, message_decoder_context_t *ctx);
extern message_decoder_context_t s_decoder_context;

static esp_err_t test_decode_post_handler(httpd_req_t *req)
{
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        const char *resp = "{\"success\":false,\"message\":\"Invalid request\"}";
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Parse hex string into bytes
    uint8_t msg_buf[256];
    int msg_len = 0;
    const char *p = content;

    while (*p != '\0' && msg_len < (int)sizeof(msg_buf)) {
        // Skip whitespace and newlines
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;

        // Parse two hex digits
        if (p[0] && p[1]) {
            char hex_byte[3] = {p[0], p[1], 0};
            char *endptr;
            unsigned long val = strtoul(hex_byte, &endptr, 16);
            if (endptr == hex_byte + 2) {
                msg_buf[msg_len++] = (uint8_t)val;
                p += 2;
            } else {
                // Invalid hex
                const char *resp = "{\"success\":false,\"message\":\"Invalid hex string\"}";
                httpd_resp_set_type(req, "application/json; charset=UTF-8");
                httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
        } else {
            break;
        }
    }

    if (msg_len == 0) {
        const char *resp = "{\"success\":false,\"message\":\"No bytes parsed\"}";
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Format message for response
    char hex_str[3 * sizeof(msg_buf) + 1];
    int pos = 0;
    for (int i = 0; i < msg_len && pos < (int)sizeof(hex_str) - 4; i++) {
        pos += snprintf(&hex_str[pos], sizeof(hex_str) - pos, "%02X ", msg_buf[i]);
    }
    hex_str[pos] = '\0';

    // Run through decoder
    bool decoded = decode_message(msg_buf, msg_len, &s_decoder_context);

    // Build JSON response (hex_str can be up to 768 bytes, plus JSON structure)
    char json_resp[1024];
    snprintf(json_resp, sizeof(json_resp),
             "{\"success\":true,\"decoded\":%s,\"length\":%d,\"hex\":\"%s\",\"message\":\"Check ESP logs for decode details\"}",
             decoded ? "true" : "false",
             msg_len,
             hex_str);

    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

// ======================================================
// URI Handlers
// ======================================================

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = home_get_handler
};

static const httpd_uri_t wifi_uri = {
    .uri = "/wifi",
    .method = HTTP_GET,
    .handler = wifi_get_handler
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

static const httpd_uri_t test_decode_uri = {
    .uri = "/api/test_decode",
    .method = HTTP_POST,
    .handler = test_decode_post_handler
};



// ======================================================
// Static File Table
// ======================================================
// To add a new static file:
//   1. Define its content as a static const char[] below
//   2. Add an entry to STATIC_FILES[]
// No other changes needed - registration is automatic.

typedef struct {
    const char *uri;
    const char *content;
    const char *content_end;    // NULL for text (strlen), non-NULL for binary (exact length)
    const char *content_type;
    const char *cache_control;  // NULL for no caching
} static_file_t;

// --- Embedded static file content (loaded from main/static/ at compile time) ---
// Symbol names are derived from the file path: slashes and dots become underscores.
// EMBED_TXTFILES adds a null terminator so the symbols can be used as C strings.

// main/static/favicon.svg  (mdi:pool, https://github.com/Templarian/MaterialDesign)
// Note: ESP-IDF derives the symbol name from the filename only, not the full path.
extern const char _binary_favicon_svg_start[];
// main/static/favicon.png  (PNG version for iOS/iPadOS, which doesn't support SVG favicons)
// Embedded as binary (EMBED_FILES) — no null terminator added.
extern const char _binary_favicon_png_start[];
extern const char _binary_favicon_png_end[];  // used to compute exact byte length
// main/static/oat.min.css + oat.min.js  (https://oat.ink, MIT license)
extern const char _binary_oat_min_css_start[];
extern const char _binary_oat_min_js_start[];

// --- File table ---

static const static_file_t STATIC_FILES[] = {
    {
        .uri           = "/static/favicon.png",
        .content       = _binary_favicon_png_start,
        .content_end   = _binary_favicon_png_end,   // binary: use exact length
        .content_type  = "image/png",
        .cache_control = "max-age=86400",
    },
    {
        .uri           = "/static/favicon.ico",
        .content       = _binary_favicon_svg_start,
        .content_end   = NULL,                      // text: use strlen
        .content_type  = "image/svg+xml",
        .cache_control = "max-age=86400",
    },
    {
        .uri           = "/static/oat.min.css",
        .content       = _binary_oat_min_css_start,
        .content_end   = NULL,
        .content_type  = "text/css",
        .cache_control = "max-age=86400",
    },
    {
        .uri           = "/static/oat.min.js",
        .content       = _binary_oat_min_js_start,
        .content_end   = NULL,
        .content_type  = "application/javascript",
        .cache_control = "max-age=86400",
    },
};
#define STATIC_FILES_COUNT (sizeof(STATIC_FILES) / sizeof(STATIC_FILES[0]))

// --- Generic handler (shared by all static files via /static/* wildcard) ---

static esp_err_t static_file_handler(httpd_req_t *req)
{
    for (int i = 0; i < STATIC_FILES_COUNT; i++) {
        if (strcmp(req->uri, STATIC_FILES[i].uri) == 0) {
            httpd_resp_set_type(req, STATIC_FILES[i].content_type);
            if (STATIC_FILES[i].cache_control) {
                httpd_resp_set_hdr(req, "Cache-Control", STATIC_FILES[i].cache_control);
            }
            ssize_t len = STATIC_FILES[i].content_end
                ? (ssize_t)(STATIC_FILES[i].content_end - STATIC_FILES[i].content)
                : HTTPD_RESP_USE_STRLEN;
            httpd_resp_send(req, STATIC_FILES[i].content, len);
            return ESP_OK;
        }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static const httpd_uri_t static_files_uri = {
    .uri     = "/static/*",
    .method  = HTTP_GET,
    .handler = static_file_handler,
};

// Redirect /favicon.ico (browser convention - requested without a <link> tag)
// to the canonical location under /static/
static esp_err_t favicon_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "/static/favicon.ico");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ======================================================
// robots.txt Handler
// ======================================================
static esp_err_t robots_txt_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "User-agent: *\r\nDisallow: /\r\n");
    return ESP_OK;
}

static const httpd_uri_t robots_txt_uri = {
    .uri     = "/robots.txt",
    .method  = HTTP_GET,
    .handler = robots_txt_handler,
};

// ======================================================
// Favicon Handler
// ======================================================
static const httpd_uri_t favicon_redirect_uri = {
    .uri     = "/favicon.ico",
    .method  = HTTP_GET,
    .handler = favicon_redirect_handler,
};

// Custom 404 error handler
// In provisioning mode: serves the WiFi config page for captive portal detection
// Otherwise: returns a standard 404
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (wifi_is_provisioning_active()) {
        ESP_LOGI(TAG, "Captive portal: redirecting %s to WiFi config", req->uri);
        return wifi_get_handler(req);
    }

    ESP_LOGI(TAG, "404: %s", req->uri);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "404 Not found");
    return ESP_FAIL;
}

// ======================================================
// Public Functions
// ======================================================

esp_err_t web_handlers_register(httpd_handle_t server)
{
    // Register main application handlers
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &scan_uri);
    httpd_register_uri_handler(server, &provision_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &status_view_uri);
    httpd_register_uri_handler(server, &mqtt_config_get_uri);
    httpd_register_uri_handler(server, &mqtt_config_post_uri);
    httpd_register_uri_handler(server, &update_get_uri);
    httpd_register_uri_handler(server, &update_post_uri);
    httpd_register_uri_handler(server, &test_decode_uri);
    httpd_register_uri_handler(server, &static_files_uri);   // /static/* wildcard
    httpd_register_uri_handler(server, &favicon_redirect_uri); // /favicon.ico -> /static/favicon.ico
    httpd_register_uri_handler(server, &robots_txt_uri);

    // Register custom 404 error handler for captive portal
    // This catches all unmatched URIs and redirects to the provisioning page
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    ESP_LOGI(TAG, "Web/HTTP handlers registered (with captive portal 404 redirect)");
    return ESP_OK;
}

