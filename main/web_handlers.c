#include "web_handlers.h"
#include "config.h"
#include "wifi_provisioning.h"
#include "pool_state.h"
#include "mqtt_poolclient.h"
#include "mqtt_discovery.h"
#include "message_decoder.h"
#include "unknown_buffer.h"
#include "device_serial.h"
#include "firmware_update.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "cJSON.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "WEB_HANDLERS";

#define _STR(x)  #x
#define STR(x)   _STR(x)

// Forward declaration - defined in main.c
const char* get_gateway_comms_status_text(uint16_t code);

// ======================================================
// HTML escaping
// ======================================================

// Returns a heap-allocated copy of `input` with &, <, >, ", ' replaced by
// their HTML entities. Caller must free(). Returns NULL on alloc failure.
static char *html_escape(const char *input)
{
    if (!input) return NULL;
    size_t len = 0;
    for (const char *p = input; *p; p++) {
        switch (*p) {
            case '&':  len += 5; break; // &amp;
            case '<':  len += 4; break; // &lt;
            case '>':  len += 4; break; // &gt;
            case '"':  len += 6; break; // &quot;
            case '\'': len += 5; break; // &#39;
            default:   len += 1; break;
        }
    }
    char *out = malloc(len + 1);
    if (!out) return NULL;
    char *q = out;
    for (const char *p = input; *p; p++) {
        switch (*p) {
            case '&':  memcpy(q, "&amp;",  5); q += 5; break;
            case '<':  memcpy(q, "&lt;",   4); q += 4; break;
            case '>':  memcpy(q, "&gt;",   4); q += 4; break;
            case '"':  memcpy(q, "&quot;", 6); q += 6; break;
            case '\'': memcpy(q, "&#39;",  5); q += 5; break;
            default:   *q++ = *p;              break;
        }
    }
    *q = '\0';
    return out;
}

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
        "<li><a href='/unknown_msgs_view'" "%s" ">Unknown Messages</a></li>"
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
        page == 'x' ? cur : none,
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
        page == 'x' ? cur : none,
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
    static const char sys_table_fmt[] =
        "<h1>Pool Controller</h1>"
        "<h2>System</h2>"
        "<table><tbody id='sys-body'>"
        "<tr><th>Serial</th><td>%s</td></tr>"
        "<tr><th>Firmware</th><td>%s</td></tr>"
        "<tr><th>Project</th><td>%s</td></tr>"
        "<tr><th>Built</th><td>%s %s</td></tr>"
        "<tr><th>Uptime</th><td>%s</td></tr>"
        "<tr><th>IP Address</th><td>%s</td></tr>"
        "<tr><th>Hostname</th><td><a href='http://%s.local/'>%s.local</a></td></tr>";
    const char *device_ip = wifi_get_device_ip();
    int sys_table_len = snprintf(NULL, 0, sys_table_fmt,
        serial,
        app_desc->version, app_desc->project_name,
        app_desc->date, app_desc->time,
        uptime_str, device_ip,
        mdns_host, mdns_host);
    char *sys_table = (sys_table_len > 0) ? malloc((size_t)sys_table_len + 1) : NULL;
    if (sys_table) {
        snprintf(sys_table, (size_t)sys_table_len + 1, sys_table_fmt,
            serial,
            app_desc->version, app_desc->project_name,
            app_desc->date, app_desc->time,
            uptime_str, device_ip,
            mdns_host, mdns_host);
    }

    // WiFi row
    char wifi_ssid[33];
    memcpy(wifi_ssid, ap_info.ssid, sizeof(wifi_ssid) - 1);
    wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
    char *wifi_ssid_esc = html_escape(wifi_ssid);
    static const char wifi_row_fmt[] = "<tr><th>WiFi</th><td>%s (%d dBm)</td></tr>";
    static const char wifi_row_none[] = "<tr><th>WiFi</th><td>Not connected</td></tr>";
    int wifi_row_len = wifi_info_ok ? snprintf(NULL, 0, wifi_row_fmt, wifi_ssid_esc ? wifi_ssid_esc : "", ap_info.rssi) : (int)sizeof(wifi_row_none) - 1;
    char *wifi_row = (wifi_row_len > 0) ? malloc((size_t)wifi_row_len + 1) : NULL;
    if (wifi_row) {
        if (wifi_info_ok) {
            snprintf(wifi_row, (size_t)wifi_row_len + 1, wifi_row_fmt, wifi_ssid_esc ? wifi_ssid_esc : "", ap_info.rssi);
        } else {
            memcpy(wifi_row, wifi_row_none, (size_t)wifi_row_len + 1);
        }
    }
    free(wifi_ssid_esc);

    // MQTT row
    char *mqtt_broker_esc = html_escape(mqtt_config.broker);
    static const char mqtt_row_fmt[] = "<tr><th>MQTT</th><td>%s (%s)</td></tr></tbody></table>";
    static const char mqtt_row_disabled[] = "<tr><th>MQTT</th><td>Disabled</td></tr></tbody></table>";
    const char *mqtt_state_str = mqtt_connected ? "Connected" : "Disconnected";
    int mqtt_row_len = mqtt_enabled
        ? snprintf(NULL, 0, mqtt_row_fmt, mqtt_state_str, mqtt_broker_esc ? mqtt_broker_esc : "")
        : (int)sizeof(mqtt_row_disabled) - 1;
    char *mqtt_row = (mqtt_row_len > 0) ? malloc((size_t)mqtt_row_len + 1) : NULL;
    if (mqtt_row) {
        if (mqtt_enabled) {
            snprintf(mqtt_row, (size_t)mqtt_row_len + 1, mqtt_row_fmt, mqtt_state_str, mqtt_broker_esc ? mqtt_broker_esc : "");
        } else {
            memcpy(mqtt_row, mqtt_row_disabled, (size_t)mqtt_row_len + 1);
        }
    }
    free(mqtt_broker_esc);

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
        "const deg='\u00b0'+(data.temperature.scale==='Fahrenheit'?'F':'C');"
        "data.devices.forEach(d=>{"
        "if(d.temperature1==null)return;"
        "const multi='temperature2' in d;"
        "rows.push([d.name+(multi?' Temperature 1':' Temperature'),d.temperature1+deg]);"
        "if(d.temperature2!=null)rows.push([d.name+' Temperature 2',d.temperature2+deg]);});"
        "data.heaters.forEach(h=>rows.push(['Heater '+(h.index+1),h.state]));"
        "data.channels.forEach(ch=>rows.push([ch.name||ch.type,ch.state]));"
        "data.lighting.forEach(lt=>{"
        "let s=lt.state;if(lt.active&&lt.color)s+=': '+lt.color;"
        "rows.push(['Lighting zone '+lt.zone,s]);});"
        "if(data.multicolor_light_type)rows.push(['Multicolor Light Type',data.multicolor_light_type]);"
        "const c=data.chlorinator;"
        "if(c.ph_reading!==null)rows.push(['pH',c.ph_reading+' (setpoint '+c.ph_setpoint+')']);"
        "if(c.orp_reading!==null)rows.push(['ORP',c.orp_reading+'mV (setpoint '+c.orp_setpoint+'mV)']);"
        "if(c.chlor_output_level!==null)rows.push(['Chlorine Output Level',c.chlor_output_level]);"
        "if(data.timers&&data.timers.length>0){"
        "data.timers.forEach(t=>rows.push(['Timer '+t.num,t.start+' \u2013 '+t.stop+' ['+t.days+']']));}"
        "const mc=data.message_counts;"
        "if(mc){"
        "const tot=mc.decoded+mc.unknown;"
        "const pct=tot>0?(mc.decoded/tot*100).toFixed(1)+'%':'n/a';"
        "const mtr=document.createElement('tr');"
        "mtr.innerHTML='<th>Messages</th><td>'+mc.decoded+' decoded, '+mc.unknown+' unknown ('+pct+')</td>';"
        "document.getElementById('sys-body').appendChild(mtr);}"
        "const rs=data.resyncs;"
        "if(rs){"
        "const parts=[];"
        "if(rs.no_start)parts.push(rs.no_start+' no-start');"
        "if(rs.header_checksum)parts.push(rs.header_checksum+' header-checksum');"
        "if(rs.bad_frame_type)parts.push(rs.bad_frame_type+' bad-frame-type');"
        "if(rs.bad_length)parts.push(rs.bad_length+' bad-length');"
        "if(rs.bad_end)parts.push(rs.bad_end+' bad-end');"
        "if(rs.data_checksum)parts.push(rs.data_checksum+' data-checksum');"
        "if(rs.buffer_overflow)parts.push(rs.buffer_overflow+' overflow');"
        "const rtr=document.createElement('tr');"
        "rtr.innerHTML='<th>Resyncs</th><td>'+(rs.total||0)+(parts.length?' ('+parts.join(', ')+')':'')+'</td>';"
        "document.getElementById('sys-body').appendChild(rtr);}"
        "const mem=data.memory;"
        "if(mem){"
        "const memtr=document.createElement('tr');"
        "memtr.innerHTML='<th>Memory</th><td>'+mem.free_heap+' free (min '+mem.min_free_heap+')</td>';"
        "document.getElementById('sys-body').appendChild(memtr);}"
        "const tb=document.getElementById('pool-body');"
        "rows.forEach(([k,v])=>{"
        "const tr=document.createElement('tr');"
        "const th=document.createElement('th');th.textContent=k;"
        "const td=document.createElement('td');td.textContent=v;"
        "tr.appendChild(th);tr.appendChild(td);"
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
    if (sys_table) httpd_resp_send_chunk(req, sys_table, HTTPD_RESP_USE_STRLEN);
    if (wifi_row)  httpd_resp_send_chunk(req, wifi_row,  HTTPD_RESP_USE_STRLEN);
    if (mqtt_row)  httpd_resp_send_chunk(req, mqtt_row,  HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, pool_section, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, footer, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);

    free(mqtt_row);
    free(wifi_row);
    free(sys_table);
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
    "<button type='button' id='retryScan' hidden onclick='scanWiFi()'>Retry Scan</button>"
    "<div id='status' role='alert' hidden class='mt-4'></div>"
    "</form>"
    "<script>"
    "function showStatus(msg,isError){"
        "const s=document.getElementById('status');"
        "s.textContent=msg;"
        "s.setAttribute('data-variant',isError?'danger':'success');"
        "s.removeAttribute('hidden');}"
    // /scan answers 202 while the scan is still running; poll until the list arrives.
    "function scanWiFi(){"
        "const s=document.getElementById('ssid');"
        "s.innerHTML='<option value=\"\">Scanning...</option>';"
        "document.getElementById('retryScan').setAttribute('hidden','');"
        "const poll=()=>fetch('/scan')"
        ".then(r=>{if(r.status===202)return new Promise(ok=>setTimeout(ok," STR(WIFI_SCAN_POLL_MS) ")).then(poll);"
        "if(!r.ok)throw new Error('Scan failed ('+r.status+')');return r.json();});"
        "poll().then(data=>{"
            "s.innerHTML='';"
            "data.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;"
            "o.text=n.ssid+' ('+n.rssi+' dBm)'+(n.current?' \\u2713':'');"
            "if(n.current)o.selected=true;s.appendChild(o);});})"
        ".catch(e=>{"
            "s.innerHTML='<option value=\"\">No networks found</option>';"
            "showStatus(e.message,true);"
            "document.getElementById('retryScan').removeAttribute('hidden');});}"
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

// The scan runs in the background so it never holds the (single-threaded)
// HTTP server for its several seconds: GET /scan starts one and answers 202,
// the page polls, and once WIFI_EVENT_SCAN_DONE has fired the next poll gets
// the results.
enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE };
static atomic_int s_scan_state = SCAN_IDLE;
static int64_t s_scan_started_us;   // Only touched by the HTTP server task

static void scan_done_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    int expected = SCAN_RUNNING;
    atomic_compare_exchange_strong(&s_scan_state, &expected, SCAN_DONE);
}

static esp_err_t scan_send_results(httpd_req_t *req)
{
    // Load current WiFi SSID from the driver to mark it in scan results.
    char current_ssid[33] = {0};
    wifi_config_t wifi_cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
        strncpy(current_ssid, (char *)wifi_cfg.sta.ssid, sizeof(current_ssid) - 1);
    }
    ESP_LOGI(TAG, "Current SSID: '%s'", current_ssid);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    esp_err_t get_err = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (get_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get scan results: %s", esp_err_to_name(get_err));
        free(ap_list);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get scan results");
        return ESP_FAIL;
    }

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

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    int state = atomic_load(&s_scan_state);
    bool stale = (esp_timer_get_time() - s_scan_started_us) > (int64_t)WIFI_SCAN_MAX_AGE_MS * 1000;

    if (state == SCAN_DONE && !stale) {
        atomic_store(&s_scan_state, SCAN_IDLE);
        return scan_send_results(req);
    }

    // A scan left over from an abandoned page, or one whose done event never
    // came, is discarded and redone.
    if (state == SCAN_IDLE || stale) {
        if (state == SCAN_DONE) {
            esp_wifi_clear_ap_list();
        }

        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = WIFI_SCAN_TIME_MIN_MS,
            .scan_time.active.max = WIFI_SCAN_TIME_MAX_MS,
        };

        // Marked running before starting, so a fast done event isn't missed.
        atomic_store(&s_scan_state, SCAN_RUNNING);
        s_scan_started_us = esp_timer_get_time();
        esp_err_t scan_err = esp_wifi_scan_start(&scan_config, false);
        if (scan_err != ESP_OK) {
            atomic_store(&s_scan_state, SCAN_IDLE);
            ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(scan_err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi scan failed");
            return ESP_FAIL;
        }
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_sendstr(req, "{\"scanning\":true}");
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
    pool_state_t state = s_pool_state;
    xSemaphoreGive(s_pool_state_mutex);

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
    cJSON_AddNumberToObject(msg_counts, "decoded", state.messages_decoded_total);
    cJSON_AddNumberToObject(msg_counts, "unknown", state.messages_unknown_total);
    cJSON_AddItemToObject(root, "message_counts", msg_counts);

    // Frame resync counters (bridge reassembly layer; see pool_state.h)
    cJSON *resyncs = cJSON_CreateObject();
    cJSON_AddNumberToObject(resyncs, "total",           state.resyncs_total);
    cJSON_AddNumberToObject(resyncs, "no_start",        state.resyncs_no_start);
    cJSON_AddNumberToObject(resyncs, "header_checksum", state.resyncs_bad_header_checksum);
    cJSON_AddNumberToObject(resyncs, "bad_frame_type",  state.resyncs_bad_frame_type);
    cJSON_AddNumberToObject(resyncs, "bad_length",      state.resyncs_bad_length);
    cJSON_AddNumberToObject(resyncs, "bad_end",         state.resyncs_bad_end);
    cJSON_AddNumberToObject(resyncs, "data_checksum",   state.resyncs_bad_data_checksum);
    cJSON_AddNumberToObject(resyncs, "buffer_overflow", state.resyncs_buffer_overflow);
    cJSON_AddNumberToObject(resyncs, "unexpected",      state.resyncs_unexpected);
    cJSON_AddItemToObject(root, "resyncs", resyncs);

    // Devices observed on the bus
    cJSON *devices = cJSON_CreateArray();
    for (int i = 0; i < state.num_seen_devices; i++) {
        const seen_device_t *d = &state.seen_devices[i];
        char addr_str[8], name_buf[16];
        snprintf(addr_str, sizeof(addr_str), "0x%02X%02X", d->addr_hi, d->addr_lo);
        const char *name = get_device_name(d->addr_hi, d->addr_lo, name_buf, sizeof(name_buf));
        cJSON *dev = cJSON_CreateObject();
        cJSON_AddStringToObject(dev, "address", addr_str);
        cJSON_AddStringToObject(dev, "name",    name);
        if (d->fw_version_valid) {
            char fw_str[16];
            snprintf(fw_str, sizeof(fw_str), "%d.%d", d->fw_version_major, d->fw_version_minor);
            cJSON_AddStringToObject(dev, "firmware_version", fw_str);
        } else {
            cJSON_AddNullToObject(dev, "firmware_version");
        }

        // Temperatures hosted by this device — only emitted for devices that
        // have actually broadcast a CMD 0x16 reading (i.e. once temp1_valid
        // first goes true, signalling that this source is a temperature
        // sensor). `temperature2` only appears for multi-sensor sources
        // (LEN 0x0E payloads).
        if (d->temp1_valid) {
            cJSON_AddNumberToObject(dev, "temperature1", d->temp1);
            if (!d->single_sensor_source) {
                if (d->temp2_valid) {
                    cJSON_AddNumberToObject(dev, "temperature2", d->temp2);
                } else {
                    cJSON_AddNullToObject(dev, "temperature2");
                }
            }
        }

        cJSON *dev_counts = cJSON_CreateObject();
        cJSON_AddNumberToObject(dev_counts, "decoded", d->decoded_count);
        cJSON_AddNumberToObject(dev_counts, "unknown", d->unknown_count);
        cJSON_AddItemToObject(dev, "message_counts", dev_counts);
        cJSON_AddItemToArray(devices, dev);
    }
    cJSON_AddItemToObject(root, "devices", devices);

    // Temperature scale (per-heater setpoints live in the heaters[] array below;
    // current readings live per-device above).
    cJSON *temperature = cJSON_CreateObject();
    cJSON_AddStringToObject(temperature, "scale", state.temp_scale_fahrenheit ? "Fahrenheit" : "Celsius");
    cJSON_AddItemToObject(root, "temperature", temperature);

    // Heaters (include heaters that have on/off state and/or setpoints)
    cJSON *heaters_arr = cJSON_CreateArray();
    for (int i = 0; i < MAX_HEATERS; i++) {
        if (!state.heaters[i].valid && !state.heaters[i].setpoint_valid) {
            continue;
        }
        cJSON *heater = cJSON_CreateObject();
        cJSON_AddNumberToObject(heater, "index", i);
        if (state.heaters[i].valid) {
            cJSON_AddStringToObject(heater, "state", state.heaters[i].on ? "On" : "Off");
        }
        if (state.heaters[i].gas_heater_valid) {
            const pool_heater_t *h = &state.heaters[i];
            cJSON *gs = cJSON_CreateObject();
            cJSON_AddStringToObject(gs, "status",
                h->status < HEATER_STATUS_NAME_COUNT ? HEATER_STATUS_NAMES[h->status] : "unknown");
            cJSON_AddBoolToObject(gs, "water_flow", h->water_flow_detected);
            cJSON_AddStringToObject(gs, "burner",
                h->burner_state < BURNER_STATE_NAME_COUNT ? BURNER_STATE_NAMES[h->burner_state] : "unknown");
            cJSON_AddBoolToObject(gs, "locked_out", h->locked_out);
            cJSON_AddBoolToObject(gs, "general_service_required", h->general_service_required);
            cJSON_AddBoolToObject(gs, "ignition_service_required", h->ignition_service_required);
            cJSON_AddBoolToObject(gs, "cooling_available", h->cooling_available);
            cJSON_AddItemToObject(heater, "gas_status", gs);
        }
        if (state.heaters[i].setpoint_valid) {
            cJSON_AddNumberToObject(heater, "pool_setpoint",   state.heaters[i].pool_setpoint);
            cJSON_AddNumberToObject(heater, "spa_setpoint",    state.heaters[i].spa_setpoint);
            cJSON_AddNumberToObject(heater, "pool_setpoint_f", state.heaters[i].pool_setpoint_f);
            cJSON_AddNumberToObject(heater, "spa_setpoint_f",  state.heaters[i].spa_setpoint_f);
        }
        cJSON_AddItemToArray(heaters_arr, heater);
    }
    cJSON_AddItemToObject(root, "heaters", heaters_arr);

    // Mode
    if (state.mode_valid) {
        const char *mode_str = (state.mode == MODE_SPA) ? "Spa" :
                               (state.mode == MODE_POOL) ? "Pool" : "Unknown";
        cJSON_AddStringToObject(root, "mode", mode_str);
    } else {
        cJSON_AddNullToObject(root, "mode");
    }

    // Controller service mode (CMD 0x12 status bitfield, bit 1)
    if (state.service_mode_valid) {
        cJSON_AddBoolToObject(root, "service_mode", state.service_mode);
    } else {
        cJSON_AddNullToObject(root, "service_mode");
    }

    // Channels (excludes heater and light-zone channels — those appear in their own sections)
    cJSON *channels = cJSON_CreateArray();
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!state.channels[i].configured ||
            state.channels[i].type == CHANNEL_TYPE_HEATER ||
            state.channels[i].type == CHANNEL_TYPE_LIGHT_ZONE) {
            continue;
        }
        const char *type_name  = get_channel_type_name(state.channels[i].type);
        const char *state_name = (state.channels[i].state < CHANNEL_STATE_COUNT) ?
                                 CHANNEL_STATE_NAMES[state.channels[i].state] : "Unknown";
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddNumberToObject(ch, "id",    state.channels[i].id);
        cJSON_AddStringToObject(ch, "name",  state.channels[i].name[0] ? state.channels[i].name : type_name);
        cJSON_AddStringToObject(ch, "type",  type_name);
        cJSON_AddStringToObject(ch, "state", state_name);
        cJSON_AddItemToArray(channels, ch);
    }
    cJSON_AddItemToObject(root, "channels", channels);

    // Lighting
    cJSON *lighting = cJSON_CreateArray();
    for (int i = 0; i < MAX_LIGHT_ZONES; i++) {
        if (!state.lighting[i].configured) continue;
        const char *state_name = (state.lighting[i].state < LIGHTING_STATE_COUNT) ?
                                 LIGHTING_STATE_NAMES[state.lighting[i].state] : "Unknown";
        const char *color_name = (state.lighting[i].color < LIGHTING_COLOR_COUNT) ?
                                 LIGHTING_COLOR_NAMES[state.lighting[i].color] : "Unknown";
        cJSON *zone = cJSON_CreateObject();
        cJSON_AddNumberToObject(zone, "zone", state.lighting[i].zone);
        if (state.lighting[i].name_valid &&
            state.lighting[i].name_id < LIGHT_ZONE_NAME_COUNT) {
            cJSON_AddStringToObject(zone, "name", LIGHT_ZONE_NAME_TABLE[state.lighting[i].name_id]);
        } else {
            cJSON_AddNullToObject(zone, "name");
        }
        if (state.lighting[i].multicolor_valid) {
            cJSON_AddBoolToObject(zone, "multicolor", state.lighting[i].multicolor);
        } else {
            cJSON_AddNullToObject(zone, "multicolor");
        }
        cJSON_AddStringToObject(zone, "state",  state_name);
        cJSON_AddStringToObject(zone, "color",  color_name);
        cJSON_AddBoolToObject(zone,   "active", state.lighting[i].active);
        cJSON_AddItemToArray(lighting, zone);
    }
    cJSON_AddItemToObject(root, "lighting", lighting);

    // Multicolor light type (register 0xF0, slot 0x01)
    if (state.multicolor_light_type_valid) {
        char type_buf[16];
        cJSON_AddStringToObject(root, "multicolor_light_type",
                                multicolor_light_type_name(state.multicolor_light_type, type_buf, sizeof(type_buf)));
    } else {
        cJSON_AddNullToObject(root, "multicolor_light_type");
    }

    // Valves
    cJSON *valves = cJSON_CreateArray();
    for (int i = 0; i < state.num_valve_slots && i < MAX_VALVE_SLOTS; i++) {
        if (!state.valves[i].configured) continue;

        // Look up label from register_labels (reg_id 0xD0 = valve 1, 0xD1 = valve 2, ...)
        uint8_t reg_id = 0xD0 + i;
        const char *label = NULL;
        for (int j = 0; j < MAX_REGISTER_LABELS; j++) {
            if (state.register_labels[j].valid &&
                state.register_labels[j].reg_id == reg_id) {
                label = state.register_labels[j].label;
                break;
            }
        }
        char fallback[16];
        if (!label) {
            snprintf(fallback, sizeof(fallback), "Valve %d", i + 1);
            label = fallback;
        }

        const char *state_name = (state.valves[i].state < CHANNEL_STATE_COUNT) ?
                                 CHANNEL_STATE_NAMES[state.valves[i].state] : "Unknown";
        cJSON *valve = cJSON_CreateObject();
        cJSON_AddNumberToObject(valve, "id",     i + 1);
        cJSON_AddStringToObject(valve, "name",   label);
        cJSON_AddStringToObject(valve, "state",  state_name);
        cJSON_AddBoolToObject(valve,   "active", state.valves[i].active);
        cJSON_AddItemToArray(valves, valve);
    }
    cJSON_AddItemToObject(root, "valves", valves);

    // Favourites
    cJSON *favourites = cJSON_CreateArray();
    for (int i = 0; i < MAX_FAVOURITES; i++) {
        const favourite_t *fav = &state.favourites[i];
        if (!fav->enabled_valid || !fav->enabled) continue;
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "index", i);
        cJSON_AddStringToObject(f, "name", fav->name_valid ? fav->name : "");
        cJSON_AddItemToArray(favourites, f);
    }
    cJSON_AddItemToObject(root, "favourites", favourites);

    // Chlorinator
    cJSON *chlorinator = cJSON_CreateObject();
    if (state.ph_valid) {
        cJSON_AddNumberToObject(chlorinator, "ph_setpoint", state.ph_setpoint / 10.0);
        cJSON_AddNumberToObject(chlorinator, "ph_reading",  state.ph_reading  / 10.0);
    } else {
        cJSON_AddNullToObject(chlorinator, "ph_setpoint");
        cJSON_AddNullToObject(chlorinator, "ph_reading");
    }
    if (state.orp_valid) {
        cJSON_AddNumberToObject(chlorinator, "orp_setpoint", state.orp_setpoint);
        cJSON_AddNumberToObject(chlorinator, "orp_reading",  state.orp_reading);
    } else {
        cJSON_AddNullToObject(chlorinator, "orp_setpoint");
        cJSON_AddNullToObject(chlorinator, "orp_reading");
    }
    if (state.chlor_output_level_valid) {
        cJSON_AddNumberToObject(chlorinator, "chlor_output_level", state.chlor_output_level);
    } else {
        cJSON_AddNullToObject(chlorinator, "chlor_output_level");
    }
    cJSON_AddItemToObject(root, "chlorinator", chlorinator);

    // Internet Gateway
    cJSON *gateway = cJSON_CreateObject();
    if (state.serial_number_valid) {
        cJSON_AddNumberToObject(gateway, "serial_number", (double)state.serial_number);
    } else {
        cJSON_AddNullToObject(gateway, "serial_number");
    }
    if (state.gateway_version_valid) {
        char fw_ver[16];
        snprintf(fw_ver, sizeof(fw_ver), "%d.%d",
                 state.gateway_version_major, state.gateway_version_minor);
        cJSON_AddStringToObject(gateway, "firmware_version", fw_ver);
    } else {
        cJSON_AddNullToObject(gateway, "firmware_version");
    }
    if (state.gateway_ip_valid) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
                 state.gateway_ip[0], state.gateway_ip[1],
                 state.gateway_ip[2], state.gateway_ip[3]);
        cJSON_AddStringToObject(gateway, "ip",           ip_str);
        cJSON_AddNumberToObject(gateway, "signal_level", state.gateway_signal_level);
    } else {
        cJSON_AddNullToObject(gateway, "ip");
        cJSON_AddNullToObject(gateway, "signal_level");
    }
    if (state.gateway_comms_status_valid) {
        cJSON_AddNumberToObject(gateway, "comms_status", state.gateway_comms_status);
        cJSON_AddStringToObject(gateway, "comms_status_text",
                                get_gateway_comms_status_text(state.gateway_comms_status));
    } else {
        cJSON_AddNullToObject(gateway, "comms_status");
        cJSON_AddNullToObject(gateway, "comms_status_text");
    }
    cJSON_AddItemToObject(root, "internet_gateway", gateway);

    // Touchscreen
    cJSON *touchscreen = cJSON_CreateObject();
    if (state.touchscreen_version_valid) {
        char ts_ver[16];
        snprintf(ts_ver, sizeof(ts_ver), "%d.%d",
                 state.touchscreen_version_major, state.touchscreen_version_minor);
        cJSON_AddStringToObject(touchscreen, "version", ts_ver);
    } else {
        cJSON_AddNullToObject(touchscreen, "version");
    }
    cJSON_AddNumberToObject(touchscreen, "num_channels", state.num_channels);
    cJSON_AddItemToObject(root, "touchscreen", touchscreen);

    // Timers (only non-empty entries)
    cJSON *timers = cJSON_CreateArray();
    for (int i = 0; i < MAX_TIMERS; i++) {
        timer_state_t *t = &state.timers[i];
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

    // Pump
    cJSON *pump = cJSON_CreateObject();
    if (state.pump_speed_valid) {
        cJSON_AddNumberToObject(pump, "speed_rpm", state.pump_speed);
    } else {
        cJSON_AddNullToObject(pump, "speed_rpm");
    }
    if (state.pump_power_watts_valid) {
        cJSON_AddNumberToObject(pump, "power_watts", state.pump_power_watts);
    } else {
        cJSON_AddNullToObject(pump, "power_watts");
    }
    cJSON_AddItemToObject(root, "pump", pump);

    // Memory
    char free_heap_str[24], min_free_heap_str[24];
    uint32_t free_heap     = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    snprintf(free_heap_str,     sizeof(free_heap_str),     "%lu.%lu KB",
             (unsigned long)(free_heap / 1024),     (unsigned long)((free_heap % 1024) * 10 / 1024));
    snprintf(min_free_heap_str, sizeof(min_free_heap_str), "%lu.%lu KB",
             (unsigned long)(min_free_heap / 1024), (unsigned long)((min_free_heap % 1024) * 10 / 1024));
    cJSON *memory = cJSON_CreateObject();
    cJSON_AddStringToObject(memory, "free_heap",     free_heap_str);
    cJSON_AddStringToObject(memory, "min_free_heap", min_free_heap_str);
    cJSON_AddItemToObject(root, "memory", memory);

    // Timestamps
    uint64_t current_tick_ms = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    cJSON_AddNumberToObject(root, "last_update_ms", (double)state.last_update_ms);
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
    if (state.last_update_ms == 0) {
        snprintf(age_str, sizeof(age_str), "Never");
    } else {
        uint32_t elapsed_sec = (uint32_t)((current_tick_ms - state.last_update_ms) / 1000);
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

    char *broker_esc   = html_escape(config.broker);
    char *username_esc = html_escape(config.username);
    const char *broker_val   = broker_esc   ? broker_esc   : "";
    const char *username_val = username_esc ? username_esc : "";
    const char *pw_placeholder = strlen(config.password) > 0
        ? "Leave empty to keep current password" : "Leave empty if not required";
    static const char html_fields_fmt[] =
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
        "</div>";
    int html_fields_len = snprintf(NULL, 0, html_fields_fmt,
        config.enabled ? " checked" : "",
        broker_val, display_port, username_val, pw_placeholder);
    char *html_fields = (html_fields_len > 0) ? malloc((size_t)html_fields_len + 1) : NULL;
    if (html_fields) {
        snprintf(html_fields, (size_t)html_fields_len + 1, html_fields_fmt,
            config.enabled ? " checked" : "",
            broker_val, display_port, username_val, pw_placeholder);
    }
    free(username_esc);
    free(broker_esc);

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
    if (html_fields) httpd_resp_send_chunk(req, html_fields, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, html_end, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, footer, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    free(html_fields);
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

        // ---- Update from GitHub -------------------------------------------
        "<h2 class='mt-4'>Update from GitHub</h2>"
        "<div id='ghStatus' role='alert'>Checking for the latest release&hellip;</div>"
        "<progress id='ghProgress' value='0' max='100' hidden class='mt-2'></progress>"
        "<div id='ghVersionWrap' hidden class='mt-2' data-field>"
        "<label for='ghVersion'>Version to install</label>"
        "<select id='ghVersion'></select>"
        "</div>"
        "<div class='mt-2'>"
        "<button type='button' id='ghCheck'>Check for updates</button> "
        "<button type='button' id='ghInstall' hidden data-variant='success'>Install selected version</button>"
        "</div>"
        "<script>"
        // Shared by the GitHub, manual-upload, reboot and entity-reset flows:
        // count down while the device reboots, then reload the page.
        "function rebootReload(el,msg){"
        "msg=msg||'Update successful!';"
        "el.setAttribute('data-variant','success');"
        "let countdown=15;"
        "el.textContent=msg+' Rebooting... reload in '+countdown+' seconds';"
        "const timer=setInterval(function(){"
        "countdown--;"
        "if(countdown>0){"
        "el.textContent=msg+' Rebooting... reload in '+countdown+' seconds';"
        "}else{"
        "clearInterval(timer);"
        "el.textContent=msg+' Rebooting... reloading now...';"
        "window.location.reload();"
        "}},1000);}"
        "(function(){"
        "const s=document.getElementById('ghStatus');"
        "const p=document.getElementById('ghProgress');"
        "const vw=document.getElementById('ghVersionWrap');"
        "const vs=document.getElementById('ghVersion');"
        "const bC=document.getElementById('ghCheck');"
        "const bI=document.getElementById('ghInstall');"
        "let polling=false;let last='';let rebooting=false;"
        // Populate the version dropdown (newest first) only when the list
        // changes, so an in-progress user selection isn't reset by polling.
        "function fill(list){"
        "const sig=(list||[]).join(',');"
        "if(vs.dataset.sig===sig)return;"
        "vs.dataset.sig=sig;vs.innerHTML='';"
        "(list||[]).forEach(function(v,i){"
        "const o=document.createElement('option');"
        "o.value=v;o.textContent=v+(i===0?' (latest)':'');vs.appendChild(o);});}"
        "function render(d){"
        "if(d.state==='downloading'){"
        "p.removeAttribute('hidden');p.value=d.progress||0;"
        "s.removeAttribute('data-variant');"
        "s.textContent='Downloading update '+(d.progress||0)+'%… do not power off.';"
        "vw.setAttribute('hidden','');bC.setAttribute('hidden','');bI.setAttribute('hidden','');return;}"
        "if(d.state==='success'){p.removeAttribute('hidden');p.value=100;"
        "vw.setAttribute('hidden','');bC.setAttribute('hidden','');bI.setAttribute('hidden','');"
        "if(!rebooting){rebooting=true;rebootReload(s);}return;}"
        "p.setAttribute('hidden','');bC.removeAttribute('hidden');"
        "const have=d.versions&&d.versions.length;"
        "if(have){fill(d.versions);vw.removeAttribute('hidden');bI.removeAttribute('hidden');}"
        "else{vw.setAttribute('hidden','');bI.setAttribute('hidden','');}"
        "if(d.state==='error'&&d.error){s.setAttribute('data-variant','danger');"
        "s.textContent='Error: '+d.error;}"
        "else if(d.update_available){s.setAttribute('data-variant','warning');"
        "let msg='Update available: '+d.latest+' (installed '+d.installed+').';"
        "if(d.release_url){s.innerHTML='';s.appendChild(document.createTextNode(msg+' '));"
        "const a=document.createElement('a');a.href=d.release_url;a.target='_blank';"
        "a.textContent='Release notes';s.appendChild(a);}else{s.textContent=msg;}}"
        "else if(d.checked){s.setAttribute('data-variant','success');"
        "s.textContent='Up to date ('+d.installed+'). You can reinstall or roll back below.';}"
        "else{s.removeAttribute('data-variant');"
        "s.textContent='Latest release not yet checked.';}}"
        "function poll(){fetch('/update/check').then(r=>r.json()).then(function(d){"
        "last=d.state;render(d);"
        "if(d.state==='downloading'||d.state==='checking'){polling=true;setTimeout(poll,2000);}"
        "else{polling=false;}"
        // A failed poll right after a download means the device rebooted
        // before we saw the 'success' state — treat it as success.
        "}).catch(function(){polling=false;"
        "if(last==='downloading')render({state:'success'});});}"
        "bC.addEventListener('click',function(){"
        "s.removeAttribute('data-variant');s.textContent='Checking for the latest release…';"
        "fetch('/update/check?refresh=1').then(r=>r.json()).then(render)"
        ".catch(function(){s.setAttribute('data-variant','danger');s.textContent='Check failed.';});});"
        "bI.addEventListener('click',function(){"
        "const v=vs.value;if(!v)return;"
        "if(!confirm('Download and install firmware '+v+' from GitHub?'))return;"
        "fetch('/update/github?version='+encodeURIComponent(v),{method:'POST'}).then(r=>r.json()).then(function(d){"
        "if(d.success){if(!polling)poll();}"
        "else{s.setAttribute('data-variant','danger');s.textContent='Error: '+(d.message||'could not start');}});});"
        "poll();"
        "})();"
        "</script>"

        // ---- Manual upload -------------------------------------------------
        "<h2 class='mt-4'>Manual upload</h2>"
        "<form id='updateForm' class='mt-2'>"
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
        "rebootReload(status);"
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
        "</script>"

        // ---- Reboot --------------------------------------------------------
        "<h2 class='mt-4'>Reboot</h2>"
        "<p>Restart the device without changing the firmware.</p>"
        "<button type='button' id='rebootBtn' data-variant='danger'>Reboot device</button>"
        "<div id='rebootStatus' role='alert' hidden class='mt-4'></div>"
        "<script>"
        "document.getElementById('rebootBtn').addEventListener('click',function(){"
        "if(!confirm('Reboot the device now?'))return;"
        "const s=document.getElementById('rebootStatus');"
        "s.removeAttribute('hidden');"
        // A network error means the device restarted before the response
        // arrived, so treat it the same as success.
        "fetch('/reboot',{method:'POST'}).then(r=>r.json()).then(function(d){"
        "rebootReload(s,'Reboot requested.');"
        "}).catch(function(){rebootReload(s,'Reboot requested.');});"
        "});"
        "</script>"

        // ---- Home Assistant entities ---------------------------------------
        "<h2 class='mt-4'>Home Assistant entities</h2>"
        "<p>Delete this device's entities in Home Assistant and re-create them, "
        "so they pick up the current entity ID naming. Any dashboards, "
        "automations or history that reference the old entity IDs will need "
        "updating. The device restarts to republish them.</p>"
        "<button type='button' id='haResetBtn' data-variant='danger'>Reset entities</button>"
        "<div id='haResetStatus' role='alert' hidden class='mt-4'></div>"
        "<script>"
        "document.getElementById('haResetBtn').addEventListener('click',function(){"
        "if(!confirm('Delete and re-create the Home Assistant entities? Their entity IDs will change.'))return;"
        "const s=document.getElementById('haResetStatus');"
        "s.removeAttribute('hidden');"
        "s.textContent='Clearing entities...';"
        // The device restarts once the entities are cleared, so a failed
        // request means the same thing here as it does for a reboot.
        "fetch('/ha-reset',{method:'POST'}).then(r=>r.json()).then(function(d){"
        "if(d.success){rebootReload(s,'Entities cleared.');}"
        "else{s.textContent='Error: '+d.message;s.setAttribute('data-variant','danger');}"
        "}).catch(function(){rebootReload(s,'Entities cleared.');});"
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
    if (remaining <= 0 || remaining > OTA_MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Invalid firmware size: %d (must be 1–%d bytes)", remaining, OTA_MAX_FIRMWARE_SIZE);
        free(buf);
        esp_ota_abort(ota_handle);
        const char *resp = "{\"success\":false,\"message\":\"Invalid firmware size\"}";
        httpd_resp_set_type(req, "application/json; charset=UTF-8");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int received = 0;
    ESP_LOGI(TAG, "Firmware size: %d bytes", remaining);

    while (remaining > 0) {
        size_t recv_size = MIN(remaining, buf_size);
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
// GitHub Update Handlers
// ======================================================

static const char *fw_state_str(fw_update_state_t state)
{
    switch (state) {
        case FW_UPDATE_STATE_CHECKING:    return "checking";
        case FW_UPDATE_STATE_DOWNLOADING: return "downloading";
        case FW_UPDATE_STATE_SUCCESS:     return "success";
        case FW_UPDATE_STATE_ERROR:       return "error";
        case FW_UPDATE_STATE_IDLE:
        default:                          return "idle";
    }
}

// GET /update/check[?refresh=1] — return current GitHub update status as JSON.
// With ?refresh=1, synchronously re-query GitHub before responding.
static esp_err_t update_check_handler(httpd_req_t *req)
{
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "refresh", val, sizeof(val)) == ESP_OK &&
            strcmp(val, "1") == 0) {
            firmware_update_check_now();
        }
    }

    fw_update_status_t st;
    firmware_update_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", fw_state_str(st.state));
    cJSON_AddStringToObject(root, "installed", st.installed_version);
    cJSON_AddStringToObject(root, "latest", st.latest_version);
    cJSON_AddStringToObject(root, "release_url", st.release_url);
    cJSON_AddBoolToObject(root, "checked", st.checked);
    cJSON_AddBoolToObject(root, "update_available", st.update_available);
    cJSON_AddNumberToObject(root, "progress", st.progress_pct);
    cJSON_AddStringToObject(root, "error", st.last_error);

    // Recent releases, newest first (latest + up to 4 prior), so the UI can
    // offer installing or rolling back to a specific version.
    cJSON *versions = cJSON_CreateArray();
    for (int i = 0; i < st.version_count && i < FW_UPDATE_MAX_VERSIONS; i++) {
        cJSON_AddItemToArray(versions, cJSON_CreateString(st.versions[i]));
    }
    cJSON_AddItemToObject(root, "versions", versions);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    if (json) {
        httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        cJSON_free(json);
    } else {
        httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /update/github[?version=<tag>] — start an OTA install of a GitHub
// release. Without a version, the latest release is installed; a version must
// be one of the recent releases reported by /update/check.
static esp_err_t update_github_post_handler(httpd_req_t *req)
{
    char version[FW_UPDATE_VERSION_LEN] = {0};
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "version", version, sizeof(version));
    }

    esp_err_t err = firmware_update_start_install(version[0] ? version : NULL);
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"success\":true,\"message\":\"Update started\"}", HTTPD_RESP_USE_STRLEN);
    } else if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send(req,
            "{\"success\":false,\"message\":\"Unknown release version\"}",
            HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req,
            "{\"success\":false,\"message\":\"No release available or an update is already running\"}",
            HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// POST /reboot — restart the device.
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reboot requested via web UI");

    const char *resp = "{\"success\":true,\"message\":\"Rebooting...\"}";
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
    esp_restart();

    return ESP_OK;
}

// POST /ha-reset — clear this device's retained Home Assistant discovery
// configs and restart, so HA re-creates the entities under the current
// entity_id scheme. The device is unreachable from here on, same as /reboot.
static esp_err_t ha_reset_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Home Assistant entity reset requested via web UI");

    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    if (!mqtt_discovery_reset_start()) {
        httpd_resp_send(req,
                        "{\"success\":false,\"message\":\"MQTT is not connected\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req,
                    "{\"success\":true,\"message\":\"Resetting entities...\"}",
                    HTTPD_RESP_USE_STRLEN);
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
// Unknown Messages Handlers
// ======================================================

static esp_err_t unknown_msgs_json_handler(httpd_req_t *req)
{
    locked_unknown_buffer_t locked = unknown_buffer_lock_for_read();
    const int count = locked.count;
    const unknown_entry_t *snap = locked.entries;
    if (snap == NULL) {
        httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR, "Couldn't acquire unknown buffer lock, try again"
        );
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        unknown_buffer_unlock_after_read();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    cJSON_AddNumberToObject(root, "count", count);
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        unknown_buffer_unlock_after_read();
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    char hex_buf[UNKNOWN_BUFFER_MAX_RAW_BYTES * 3 + 1];
    for (int i = 0; i < count; i++) {
        const unknown_entry_t *e = &snap[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) continue;

        // Decode address/command fields from the raw frame
        // Frame layout: [START=0][SRC=1-2][DST=3-4][FRAME_TYPE=5-6][CMD=7][LEN=8][HDR_CHK=9][DATA=10...]
        char addr_str[8];
        char name_buf[16];
        uint8_t src_hi = (e->raw_len > 2) ? e->raw[1] : 0;
        uint8_t src_lo = (e->raw_len > 2) ? e->raw[2] : 0;
        uint8_t dst_hi = (e->raw_len > 4) ? e->raw[3] : 0;
        uint8_t dst_lo = (e->raw_len > 4) ? e->raw[4] : 0;
        uint8_t cmd    = (e->raw_len > 7) ? e->raw[7] : 0;

        snprintf(addr_str, sizeof(addr_str), "0x%02X%02X", src_hi, src_lo);
        cJSON_AddStringToObject(obj, "src", addr_str);
        cJSON_AddStringToObject(obj, "src_name",
            get_device_name(src_hi, src_lo, name_buf, sizeof(name_buf)));

        snprintf(addr_str, sizeof(addr_str), "0x%02X%02X", dst_hi, dst_lo);
        cJSON_AddStringToObject(obj, "dst", addr_str);
        cJSON_AddStringToObject(obj, "dst_name",
            get_device_name(dst_hi, dst_lo, name_buf, sizeof(name_buf)));

        char cmd_str[7];
        snprintf(cmd_str, sizeof(cmd_str), "0x%02X", cmd);
        cJSON_AddStringToObject(obj, "cmd", cmd_str);

        // Payload bytes (raw[10] to raw[raw_len-3]) as space-separated hex
        // Cap at what was actually stored — raw[] only holds the first UNKNOWN_BUFFER_MAX_RAW_BYTES bytes.
        int payload_start = 10;
        int stored_len    = ((int)e->raw_len < UNKNOWN_BUFFER_MAX_RAW_BYTES) ? (int)e->raw_len : UNKNOWN_BUFFER_MAX_RAW_BYTES;
        int payload_end   = ((int)e->raw_len - 2 < stored_len) ? (int)e->raw_len - 2 : stored_len;
        int payload_len   = (payload_end > payload_start) ? (payload_end - payload_start) : 0;
        if (payload_len > 0) {
            int pos = 0;
            for (int j = 0; j < payload_len; j++) {
                pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", e->raw[payload_start + j]);
            }
            if (pos > 0) hex_buf[pos - 1] = '\0';
            cJSON_AddStringToObject(obj, "payload", hex_buf);
        } else {
            cJSON_AddStringToObject(obj, "payload", "");
        }

        // Raw frame bytes (capped at UNKNOWN_BUFFER_MAX_RAW_BYTES) as space-separated hex
        {
            int pos = 0;
            for (int j = 0; j < stored_len; j++) {
                pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", e->raw[j]);
            }
            if (pos > 0) hex_buf[pos - 1] = '\0';
            cJSON_AddStringToObject(obj, "raw", hex_buf);
        }

        cJSON_AddBoolToObject(obj,   "is_error",   e->is_error);
        cJSON_AddStringToObject(obj, "reason",     unknown_reason_str(e->reason));
        cJSON_AddNumberToObject(obj, "raw_len",    (double)e->raw_len);
        cJSON_AddNumberToObject(obj, "hits",       (double)e->hit_count);
        cJSON_AddNumberToObject(obj, "first_seen", (double)e->first_seen);
        cJSON_AddNumberToObject(obj, "last_seen",  (double)e->last_seen);

        cJSON_AddItemToArray(arr, obj);
    }

    unknown_buffer_unlock_after_read();
    cJSON_AddItemToObject(root, "entries", arr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t unknown_msgs_clear_handler(httpd_req_t *req)
{
    char buf[32];
    int remaining = req->content_len;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (ret <= 0) break;
        remaining -= ret;
    }

    unknown_buffer_clear();

    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t unknown_msgs_view_handler(httpd_req_t *req)
{
    const char *page_content =
        "<h1>Unknown Bus Messages</h1>"
        "<p class='text-lighter'>Unrecognized and error frames captured from the bus "
        "(max " STR(UNKNOWN_BUFFER_CAPACITY) " stored). Sorted by most recently seen.</p>"
        "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:1rem'>"
        "<span id='unk-count' class='text-lighter'></span>"
        "<div style='display:flex;gap:0.5rem'>"
        "<a href='/unknown_msgs' class='outline button'>Raw JSON</a>"
        "<button id='clear-btn' class='outline' data-variant='danger'>Clear Buffer</button>"
        "</div>"
        "</div>"
        "<div id='unk-error' role='alert' data-variant='danger' hidden></div>"
        "<p id='unk-empty' hidden class='text-lighter'>No unknown messages recorded yet.</p>"
        "<div id='unk-table' hidden style='overflow-x:auto'>"
        "<table><thead><tr>"
        "<th>Source</th><th>Destination</th><th style='width:5em'>CMD</th><th>Payload</th><th>Raw Frame</th>"
        "<th style='width:4em'>Hits</th><th>First Seen</th><th>Last Seen</th>"
        "</tr></thead><tbody id='unk-tbody'></tbody></table>"
        "</div>"
        "<script>"
        "function fmtTime(ts){"
        "if(!ts||ts<=0)return'Unknown';"
        "const d=new Date(ts*1000);"
        "const tz=Intl.DateTimeFormat().resolvedOptions().timeZone;"
        "return d.toLocaleString('en-GB',{"
        "day:'2-digit',month:'2-digit',year:'numeric',"
        "hour:'2-digit',minute:'2-digit',second:'2-digit',"
        "hour12:false,timeZone:tz,timeZoneName:'short'});}"
        "function mkEl(tag,txt){"
        "const el=document.createElement(tag);"
        "if(txt!==undefined)el.textContent=txt;"
        "return el;}"
        "function addCode(td,txt){"
        "const c=document.createElement('code');c.textContent=txt;td.appendChild(c);}"
        "function addrTd(addr,name,label,isErr){"
        "const td=mkEl('td');"
        "if(label){const b=mkEl('small',label);const bg=isErr?'#e55':'#e0a800';b.style.cssText='background:'+bg+';color:#fff;border-radius:3px;padding:0 4px;margin-right:4px;font-size:.75em';td.appendChild(b);}"
        "addCode(td,addr);"
        "if(name){td.appendChild(document.createElement('br'));td.appendChild(mkEl('small',name));}"
        "return td;}"
        "function load(){"
        "fetch('/unknown_msgs').then(r=>r.json()).then(data=>{"
        "const count=data.count||0;"
        "document.getElementById('unk-count').textContent="
        "count+' unique raw frame'+(count!==1?'s':'');"
        "const empty=document.getElementById('unk-empty');"
        "const tbl=document.getElementById('unk-table');"
        "const tbody=document.getElementById('unk-tbody');"
        "tbody.innerHTML='';"
        "if(count===0){"
        "empty.removeAttribute('hidden');tbl.setAttribute('hidden','');}"
        "else{"
        "empty.setAttribute('hidden','');"
        "const entries=[...(data.entries||[])];"
        "entries.sort((a,b)=>b.last_seen-a.last_seen);"
        "entries.forEach(e=>{"
        "const tr=document.createElement('tr');"
        "if(e.is_error)tr.style.background='var(--danger-faint,#fff0f0)';"
        "tr.appendChild(addrTd(e.src,e.src_name,e.reason!=='unhandled'?e.reason:null,e.is_error));"
        "tr.appendChild(addrTd(e.dst,e.dst_name));"
        "const cmdTd=mkEl('td');addCode(cmdTd,e.cmd);tr.appendChild(cmdTd);"
        "const payTd=mkEl('td');"
        "const pc=document.createElement('code');"
        "pc.textContent=e.payload||'(empty)';"
        "pc.style.padding='0';"
        "payTd.appendChild(pc);tr.appendChild(payTd);"
        "const rawTd=mkEl('td');"
        "const rc=document.createElement('code');"
        "rc.textContent=e.raw||'(empty)';"
        "rc.style.wordBreak='break-all';"
        "rc.style.padding='0';"
        "rawTd.appendChild(rc);"
        "if(e.raw_len>" STR(UNKNOWN_BUFFER_MAX_RAW_BYTES) "){"
        "const trunc=document.createElement('small');"
        "trunc.textContent=' (first " STR(UNKNOWN_BUFFER_MAX_RAW_BYTES) " bytes of '+e.raw_len+')';"
        "trunc.style.color='var(--muted-foreground,#555)';"
        "rawTd.appendChild(trunc);}"
        "tr.appendChild(rawTd);"
        "tr.appendChild(mkEl('td',String(e.hits)));"
        "tr.appendChild(mkEl('td',fmtTime(e.first_seen)));"
        "tr.appendChild(mkEl('td',fmtTime(e.last_seen)));"
        "tbody.appendChild(tr);});"
        "tbl.removeAttribute('hidden');}}"
        ").catch(err=>{"
        "const errEl=document.getElementById('unk-error');"
        "errEl.textContent='Error loading data: '+err;"
        "errEl.removeAttribute('hidden');});}"
        "load();"
        "document.getElementById('clear-btn').addEventListener('click',function(){"
        "if(!confirm('Clear all unknown message records?'))return;"
        "fetch('/unknown_msgs/clear',{method:'POST'}).then(()=>load()).catch(err=>{"
        "const errEl=document.getElementById('unk-error');"
        "errEl.textContent='Clear failed: '+err;"
        "errEl.removeAttribute('hidden');});});"
        "</script>";

    char page_title[] = "Unknown Bus Messages";
    char *header = get_page_header(page_title);
    char *nav    = get_page_nav('x');
    char *footer = get_page_footer();

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_send_chunk(req, header,       HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nav,          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, page_content, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, footer,       HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);

    free(footer);
    free(nav);
    free(header);
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

static const httpd_uri_t update_check_uri = {
    .uri = "/update/check",
    .method = HTTP_GET,
    .handler = update_check_handler
};

static const httpd_uri_t update_github_uri = {
    .uri = "/update/github",
    .method = HTTP_POST,
    .handler = update_github_post_handler
};

static const httpd_uri_t reboot_post_uri = {
    .uri = "/reboot",
    .method = HTTP_POST,
    .handler = reboot_post_handler
};

static const httpd_uri_t ha_reset_post_uri = {
    .uri = "/ha-reset",
    .method = HTTP_POST,
    .handler = ha_reset_post_handler
};

static const httpd_uri_t test_decode_uri = {
    .uri = "/api/test_decode",
    .method = HTTP_POST,
    .handler = test_decode_post_handler
};

static const httpd_uri_t unknown_msgs_json_uri = {
    .uri = "/unknown_msgs",
    .method = HTTP_GET,
    .handler = unknown_msgs_json_handler
};

static const httpd_uri_t unknown_msgs_clear_uri = {
    .uri = "/unknown_msgs/clear",
    .method = HTTP_POST,
    .handler = unknown_msgs_clear_handler
};

static const httpd_uri_t unknown_msgs_view_uri = {
    .uri = "/unknown_msgs_view",
    .method = HTTP_GET,
    .handler = unknown_msgs_view_handler
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
    httpd_register_uri_handler(server, &update_check_uri);
    httpd_register_uri_handler(server, &update_github_uri);
    httpd_register_uri_handler(server, &reboot_post_uri);
    httpd_register_uri_handler(server, &ha_reset_post_uri);
    httpd_register_uri_handler(server, &test_decode_uri);
    httpd_register_uri_handler(server, &unknown_msgs_json_uri);
    httpd_register_uri_handler(server, &unknown_msgs_clear_uri);
    httpd_register_uri_handler(server, &unknown_msgs_view_uri);
    httpd_register_uri_handler(server, &static_files_uri);   // /static/* wildcard
    httpd_register_uri_handler(server, &favicon_redirect_uri); // /favicon.ico -> /static/favicon.ico
    httpd_register_uri_handler(server, &robots_txt_uri);

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_done_event_handler, NULL);

    // Register custom 404 error handler for captive portal
    // This catches all unmatched URIs and redirects to the provisioning page
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    ESP_LOGI(TAG, "Web/HTTP handlers registered (with captive portal 404 redirect)");
    return ESP_OK;
}

