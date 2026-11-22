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
#include "firmware_version.h"
#include "usage_report.h"
#include "web_log.h"
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

// Sends a complete JSON response.
static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// ======================================================
// Common HTML Page Parts (shell, navigation, footer)
// ======================================================

// Pages with a sidebar entry; indexes NAV_ITEMS.
typedef enum {
    PAGE_HOME,
    PAGE_WIFI,
    PAGE_MQTT,
    PAGE_FIRMWARE,
    PAGE_DEVICE,
    PAGE_STATUS,
    PAGE_UNKNOWN,
    PAGE_LOG,
    PAGE_COUNT
} web_page_t;

// Sidebar entries, in display order. A non-NULL group starts a new labelled
// section above that entry.
static const struct {
    const char *group;
    const char *uri;
    const char *icon;   // app.css .i-<icon> class
    const char *label;
} NAV_ITEMS[PAGE_COUNT] = {
    [PAGE_HOME]     = { NULL,          "/",                  "gauge",    "Dashboard" },
    [PAGE_WIFI]     = { "Settings",    "/wifi",              "wifi",     "WiFi" },
    [PAGE_MQTT]     = { NULL,          "/mqtt_config",       "mqtt",     "MQTT" },
    [PAGE_FIRMWARE] = { NULL,          "/update",            "download", "Firmware" },
    [PAGE_DEVICE]   = { NULL,          "/device",            "cpu",      "Device" },
    [PAGE_STATUS]   = { "Diagnostics", "/status_view",       "braces",   "Status JSON" },
    [PAGE_UNKNOWN]  = { NULL,          "/unknown_msgs_view", "help",     "Unknown messages" },
    [PAGE_LOG]      = { NULL,          "/log_view",          "terminal", "Debug log" },
};

// Sends `s` as one chunk of a chunked response. NULL and empty strings are
// skipped: a zero-length chunk would end the response.
static void send_str(httpd_req_t *req, const char *s)
{
    if (s && s[0] != '\0') {
        httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
    }
}

// Short hash of this build's ELF, appended to app.css/app.js links so each
// build's pages fetch that build's assets rather than a cached older copy.
static const char *asset_version(void)
{
    static char ver[9];
    if (ver[0] == '\0') {
        esp_app_get_elf_sha256(ver, sizeof(ver));
    }
    return ver;
}

// Streams the document head, top bar, sidebar and the opening <main>. `title`
// is inserted unescaped, so pass only fixed strings.
static void send_page_start(httpd_req_t *req, const char *title, web_page_t page)
{
    const char *ver = asset_version();
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    send_str(req,
        "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='color-scheme' content='light dark'>"
        "<link rel='icon' type='image/png' href='/static/favicon.png'>"
        "<link rel='icon' type='image/svg+xml' href='/static/favicon.ico'>"
        "<link rel='apple-touch-icon' href='/static/favicon.png'>"
        // Oat themes on data-theme; follow the OS setting, before first paint
        "<script>(function(){const m=matchMedia('(prefers-color-scheme: dark)');"
        "const s=()=>document.documentElement.dataset.theme=m.matches?'dark':'light';"
        "s();m.addEventListener('change',s);})();</script>"
        "<link rel='stylesheet' href='/static/oat.min.css'>"
        "<link rel='stylesheet' href='/static/app.css?v=");
    send_str(req, ver);
    send_str(req, "'><script src='/static/app.js?v=");
    send_str(req, ver);
    send_str(req, "'></script><title>");
    send_str(req, title);
    send_str(req,
        " - Pool Controller</title></head><body><div data-sidebar-layout>"
        "<nav data-topnav>"
        "<button data-sidebar-toggle aria-label='Toggle menu' class='outline'>&#9776;</button>"
        "<span class='brand'><i class='i i-pool' aria-hidden='true'></i>Pool Controller</span>"
        "</nav>"
        "<aside data-sidebar>"
        "<header class='brand'><i class='i i-pool' aria-hidden='true'></i>Pool Controller</header>"
        "<nav><ul>");
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (NAV_ITEMS[i].group) {
            send_str(req, "<li class='nav-group'>");
            send_str(req, NAV_ITEMS[i].group);
            send_str(req, "</li>");
        }
        send_str(req, "<li><a href='");
        send_str(req, NAV_ITEMS[i].uri);
        send_str(req, i == (int)page ? "' aria-current='page'>" : "'>");
        send_str(req, "<i class='i i-");
        send_str(req, NAV_ITEMS[i].icon);
        send_str(req, "' aria-hidden='true'></i>");
        send_str(req, NAV_ITEMS[i].label);
        send_str(req, "</a></li>");
    }
    send_str(req, "</ul></nav><footer><small>");
    send_str(req, esp_app_get_description()->version);
    send_str(req, "</small></footer></aside><main>");
}

// Closes the page opened by send_page_start and ends the chunked response.
static void send_page_end(httpd_req_t *req)
{
    send_str(req, "</main></div></body></html>");
    httpd_resp_send_chunk(req, NULL, 0);
}

// A page whose whole body is one fixed string.
static esp_err_t send_static_page(httpd_req_t *req, const char *title, web_page_t page,
                                  const char *body)
{
    send_page_start(req, title, page);
    send_str(req, body);
    send_page_end(req);
    return ESP_OK;
}

// ======================================================
// Dashboard (Home) Page Handler
// ======================================================

static void format_uptime(char *buf, size_t len)
{
    uint32_t uptime_s = (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    uint32_t d = uptime_s / 86400, h = (uptime_s % 86400) / 3600;
    uint32_t m = (uptime_s % 3600) / 60, s = uptime_s % 60;
    if (d > 0)      snprintf(buf, len, "%lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)m);
    else if (h > 0) snprintf(buf, len, "%luh %lum %lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    else            snprintf(buf, len, "%lum %lus", (unsigned long)m, (unsigned long)s);
}

// Connection state shown by the dashboard chips and the Device page. Strings
// are HTML-escaped copies on the heap; release with conn_status_free().
typedef struct {
    char *ssid_esc;         // NULL when the station isn't connected
    int8_t rssi;
    bool wifi_weak;         // Connected, but RSSI below WIFI_RSSI_WEAK_DBM
    bool mqtt_enabled;
    bool mqtt_connected;
    char *broker_esc;
} conn_status_t;

static void conn_status_load(conn_status_t *st)
{
    *st = (conn_status_t){0};
    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        char ssid[33];
        memcpy(ssid, ap_info.ssid, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
        st->ssid_esc = html_escape(ssid);
        st->rssi = ap_info.rssi;
        st->wifi_weak = ap_info.rssi < WIFI_RSSI_WEAK_DBM;
    }
    mqtt_config_t mqtt_config = {0};
    st->mqtt_enabled = mqtt_load_config(&mqtt_config) && mqtt_config.enabled;
    st->mqtt_connected = mqtt_client_is_connected();
    st->broker_esc = html_escape(mqtt_config.broker);
}

static void conn_status_free(conn_status_t *st)
{
    free(st->ssid_esc);
    free(st->broker_esc);
}

// The WiFi status chip: green when connected, amber when the signal is weak,
// red when not connected. A link to `href`, or plain when href is NULL (on
// the WiFi page itself).
static void send_wifi_chip(httpd_req_t *req, const conn_status_t *conn, const char *href)
{
    if (href) {
        send_str(req, "<a class='chip' href='");
        send_str(req, href);
        send_str(req, "'>");
    } else {
        send_str(req, "<span class='chip'>");
    }
    if (conn->ssid_esc) {
        char rssi[24];
        snprintf(rssi, sizeof(rssi), ", %d dBm", conn->rssi);
        send_str(req, conn->wifi_weak ? "<span class='dot warn'></span>WiFi "
                                      : "<span class='dot ok'></span>WiFi ");
        send_str(req, conn->ssid_esc);
        send_str(req, rssi);
        send_str(req, conn->wifi_weak ? " (weak)" : NULL);
    } else {
        send_str(req, "<span class='dot bad'></span>WiFi not connected");
    }
    send_str(req, href ? "</a>" : "</span>");
}

static const char DASHBOARD_PAGE_HEAD[] =
    "<div class='page-head'>"
    "<h1>Pool <span id='mode'></span></h1>"
    "<small id='updated' class='muted'></small>"
    "</div>"
    "<div class='chips'>";

static const char DASHBOARD_PAGE_BODY[] =
    "<a id='fw-chip' class='chip' href='/update' hidden></a>"
    "</div>"
    "<div id='err' role='alert' data-variant='danger' hidden class='mb-4'></div>"
    "<div id='cards' class='cards'><p class='muted'>Loading&hellip;</p></div>"
    "<script>"
    "(function(){"
    "const $=id=>document.getElementById(id);"
    "const esc=s=>{const e=document.createElement('i');e.textContent=s;return e.innerHTML;};"
    "const pill=s=>'<span class=\"pill '+(s==='Off'?'off':/^Auto/.test(s)?'auto':'')+'\">'+esc(s)+'</span>';"
    // k is escaped here; v is HTML the caller has built (escaping any device strings)
    "const row=(k,v,sub,mono)=>'<li><span>'+esc(k)+(sub?'<span class=\"sub'+(mono?' mono':'')+'\">'+esc(sub)+'</span>':'')+'</span><span>'+v+'</span></li>';"
    "const rows=r=>r?'<ul class=\"rows\">'+r+'</ul>':'';"
    "const card=(icon,title,body)=>'<section class=\"card\"><h2><i class=\"i i-'+icon+'\" aria-hidden=\"true\"></i>'+title+'</h2>'+body+'</section>';"
    "function render(d){"
    // Readings are always °C on the bus; the scale only sets how the
    // Touchscreen displays them, so convert to match it.
    "const F=d.temperature.scale==='Fahrenheit';"
    "const t=v=>F?Math.round(v*9/5+32)+'°F':v+'°C';"
    "const out=[];"
    // Temperature: the first connected sensor large, the rest and the heaters as rows
    "const temps=[];"
    "d.devices.forEach(dv=>{if(!('temperature1' in dv))return;const multi='temperature2' in dv;"
    "[1,2].forEach(n=>{if(n===2&&!multi)return;"
    "temps.push({name:dv.name+(multi?' sensor '+n:''),on:dv['temperature'+n+'_connected'],v:dv['temperature'+n]});});});"
    "const main=temps.find(x=>x.on);let h='',r='';"
    "if(main)h+='<div class=\"big\">'+t(main.v)+'</div><p class=\"muted small\">'+esc(main.name)+'</p>';"
    "temps.forEach(x=>{if(x!==main)r+=row(x.name,x.on?t(x.v):'<span class=\"muted\">Not connected</span>');});"
    "d.heaters.forEach(ht=>{const sp='pool_setpoint' in ht?(F"
    "?'Pool '+ht.pool_setpoint_f+'°F · Spa '+ht.spa_setpoint_f+'°F'"
    ":'Pool '+ht.pool_setpoint+'°C · Spa '+ht.spa_setpoint+'°C'):'';"
    "r+=row('Heater '+(ht.index+1),ht.state?pill(ht.state):'',sp);});"
    "h+=rows(r);if(h)out.push(card('temp','Temperature',h));"
    // Water chemistry
    "const c=d.chlorinator;h='';r='';"
    "const met=(v,u,lbl,sp)=>'<div class=\"metric\"><b>'+(v!=null?v+(u?'<small> '+u+'</small>':''):'—')+'</b>'"
    "+'<span class=\"muted small\">'+lbl+(sp!=null?' · target '+sp:'')+'</span></div>';"
    "const ph=v=>v!=null?v.toFixed(1):v;"
    "if(c.ph_reading!=null||c.ph_setpoint!=null||c.orp_reading!=null||c.orp_setpoint!=null)"
    "h+='<div class=\"metrics\">'+met(ph(c.ph_reading),'','pH',ph(c.ph_setpoint))+met(c.orp_reading,'mV','ORP',c.orp_setpoint)+'</div>';"
    "const rng=(lo,hi,u)=>(lo!=null?lo+u:'—')+' – '+(hi!=null?hi+u:'—');"
    "if(c.ph_lowest!=null||c.ph_highest!=null)r+=row('pH range measured',rng(c.ph_lowest,c.ph_highest,''));"
    "if(c.orp_lowest!=null||c.orp_highest!=null)r+=row('ORP range measured',rng(c.orp_lowest,c.orp_highest,' mV'));"
    "if(c.cell_running_time!=null)r+=row('Cell running time',c.cell_running_time+' h');"
    "if(c.low_salt_cell_running_time!=null)r+=row('Low salt cell running time',c.low_salt_cell_running_time+' h');"
    "if(c.previous_day_cell_load!=null)r+=row('Previous day cell load',c.previous_day_cell_load+'%');"
    "if(c.chlor_output_level!=null)r+=row('Chlorine output level',c.chlor_output_level);"
    "h+=rows(r);if(h)out.push(card('droplet','Water chemistry',h));"
    // Equipment
    "r='';const p=d.pump||{};"
    "d.channels.forEach(x=>r+=row(x.name||x.type,pill(x.state)));"
    "(d.valves||[]).forEach(x=>r+=row(x.name,pill(x.state)));"
    "if(p.speed_rpm!=null)r+=row('Pump speed',p.speed_rpm+' rpm');"
    "if(p.power_watts!=null)r+=row('Pump power',p.power_watts+' W');"
    "if(r)out.push(card('fan','Equipment',rows(r)));"
    // Lighting
    "r='';"
    "d.lighting.forEach(z=>{let s=z.state;if(z.active&&z.color)s+=' · '+z.color;r+=row(z.name||'Zone '+z.zone,pill(s));});"
    "if(d.multicolor_light_type)r+=row('Multicolor light type',esc(d.multicolor_light_type));"
    "if(r)out.push(card('bulb','Lighting',rows(r)));"
    // Timers
    "r='';"
    "(d.timers||[]).forEach(x=>r+=row(x.start+' – '+x.stop,'<span class=\"mono small muted\">'+esc(x.days)+'</span>','Timer '+x.num));"
    "if(r)out.push(card('clock','Timers',rows(r)));"
    // Devices seen on the bus, this bridge included
    "r='';"
    "d.devices.forEach(dv=>r+=row(dv.name,dv.firmware_version?'v'+esc(dv.firmware_version):"
    "'<span class=\"muted\">—</span>',dv.address,true));"
    "if(r)out.push(card('cpu','Devices',rows(r)));"
    "$('cards').innerHTML=out.length?out.join(''):"
    "'<p class=\"muted\">No pool data yet. Readings appear here as the controller reports them on the bus.</p>';"
    "$('mode').innerHTML=(d.mode?'<span class=\"pill auto\">'+esc(d.mode)+' mode</span>':'')+(d.service_mode?' <span class=\"pill warn\">Service mode</span>':'');"
    "const age=d.time_since_last_update;"
    "$('updated').textContent=!age||age==='Never'?'No bus data yet':'Bus updated '+age;}"
    "let busy=false;"
    "function load(){if(busy||document.hidden)return;busy=true;"
    "fetch('/status').then(r=>{if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
    ".then(d=>{render(d);$('err').hidden=true;})"
    ".catch(e=>showAlert($('err'),\"Couldn't load pool status: \"+e.message,'danger'))"
    ".finally(()=>{busy=false;});}"
    "load();setInterval(load," STR(WEB_DASHBOARD_REFRESH_MS) ");"
    "document.addEventListener('visibilitychange',load);"
    "fetch('/update/check').then(r=>r.json()).then(u=>{const c=$('fw-chip');"
    "if(u.update_available)c.innerHTML='<span class=\"dot warn\"></span>Update available: '+esc(u.latest);"
    "else if(u.checked)c.innerHTML='<span class=\"dot ok\"></span>Firmware up to date';"
    "else return;c.hidden=false;}).catch(()=>{});"
    "})();"
    "</script>";

static esp_err_t home_get_handler(httpd_req_t *req)
{
    conn_status_t conn;
    conn_status_load(&conn);
    char uptime[32];
    format_uptime(uptime, sizeof(uptime));

    send_page_start(req, "Dashboard", PAGE_HOME);
    send_str(req, DASHBOARD_PAGE_HEAD);

    // Connection chips, rendered here since /status doesn't carry them
    send_wifi_chip(req, &conn, "/wifi");
    if (!conn.mqtt_enabled) {
        send_str(req, "<a class='chip' href='/mqtt_config'><span class='dot'></span>MQTT off</a>");
    } else if (conn.mqtt_connected) {
        send_str(req, "<a class='chip' href='/mqtt_config'><span class='dot ok'></span>MQTT connected</a>");
    } else {
        send_str(req, "<a class='chip' href='/mqtt_config'><span class='dot bad'></span>MQTT disconnected</a>");
    }
    send_str(req, "<a class='chip' href='/device'><i class='i i-clock' aria-hidden='true'></i>Up ");
    send_str(req, uptime);
    send_str(req, "</a>");

    send_str(req, DASHBOARD_PAGE_BODY);
    send_page_end(req);
    conn_status_free(&conn);
    return ESP_OK;
}

// ======================================================
// Device Page Handler
// ======================================================

static void send_kv(httpd_req_t *req, const char *key, const char *value_html)
{
    send_str(req, "<dt>");
    send_str(req, key);
    send_str(req, "</dt><dd>");
    send_str(req, value_html);
    send_str(req, "</dd>");
}

static const char DEVICE_PAGE_HEALTH[] =
    "<dt>Messages</dt><dd id='msgs'>&hellip;</dd>"
    "<dt>Resyncs</dt><dd id='resyncs'>&hellip;</dd>"
    "<dt>Memory</dt><dd id='mem'>&hellip;</dd>"
    "</dl></section>"
    "<section class='card'><h2><i class='i i-chart' aria-hidden='true'></i>Anonymous usage reporting</h2>";

static const char DEVICE_PAGE_TAIL[] =
    "</section>"
    "<section class='card danger'><h2><i class='i i-alert' aria-hidden='true'></i>Danger zone</h2>"
    "<div class='action-row'><span>Restart the device without changing the firmware.</span>"
    "<button type='button' id='rebootBtn' class='outline' data-variant='danger'>Restart</button></div>"
    "<div class='action-row'><span>Re-create this device's Home Assistant entities with the current "
    "entity ID naming. Dashboards, automations and history using the old IDs will need updating. "
    "Resetting restarts the device.</span>"
    "<button type='button' id='haResetBtn' class='outline' data-variant='danger'>Reset entities</button></div>"
    "<div id='dangerStatus' role='alert' hidden class='mt-2'></div>"
    "</section></div>"
    "<script>"
    "fetch('/status').then(r=>r.json()).then(d=>{"
    "if(d.message_counts)document.getElementById('msgs').textContent=messageSummary(d.message_counts);"
    "if(d.resyncs)document.getElementById('resyncs').textContent=resyncSummary(d.resyncs);"
    "const m=d.memory;"
    "if(m)document.getElementById('mem').textContent=m.free_heap+' free (min '+m.min_free_heap+')';"
    "}).catch(()=>{['msgs','resyncs','mem'].forEach(id=>document.getElementById(id).textContent='Unavailable');});"
    "(function(){"
    "const s=document.getElementById('dangerStatus');"
    "document.getElementById('rebootBtn').addEventListener('click',function(){"
    "if(!confirm('Restart the device now?'))return;"
    // A network error means the device restarted before the response
    // arrived, so treat it the same as success.
    "fetch('/reboot',{method:'POST'}).then(r=>r.json()).then(function(){"
    "restartReload(s);"
    "}).catch(function(){restartReload(s);});});"
    "document.getElementById('haResetBtn').addEventListener('click',function(){"
    "if(!confirmRestart('Delete and re-create the Home Assistant entities? Their entity IDs will change.'))return;"
    "showAlert(s,'Clearing entities...');"
    // The device restarts once the entities are cleared, so a failed
    // request means the same thing here as it does for a reboot.
    "fetch('/ha-reset',{method:'POST'}).then(r=>r.json()).then(function(d){"
    "if(d.success){restartReload(s,'Entities cleared.');}"
    "else{showAlert(s,'Error: '+d.message,'danger');}"
    "}).catch(function(){restartReload(s,'Entities cleared.');});});"
    "})();"
    "</script>";

static const char USAGE_REPORT_DESC[] =
    "<p>Help shape the project by showing how many devices run each "
    "release. Twice a day this device shares a random install ID and its "
    "firmware version, never anything about your pool, network or hardware. "
    "<a href='" USAGE_REPORT_README_URL "' target='_blank'>What is shared</a></p>";

// Forward declaration - shared with the firmware page below
static void send_settings_toggle(httpd_req_t *req, const char *id, const char *url,
                                 const char *label, bool checked, const char *desc_html);

static esp_err_t device_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char buf[160];

    conn_status_t conn;
    conn_status_load(&conn);

    send_page_start(req, "Device", PAGE_DEVICE);
    send_str(req, "<div class='page-head'><h1>Device</h1></div>"
                  "<div class='cards'>"
                  "<section class='card'><h2><i class='i i-cpu' aria-hidden='true'></i>System</h2><dl class='kv'>");

    const char *mdns_host = wifi_get_mdns_hostname();
    snprintf(buf, sizeof(buf), "<a href='http://%s.local/'>%s.local</a>", mdns_host, mdns_host);
    send_kv(req, "Hostname", buf);
    send_kv(req, "IP address", wifi_get_device_ip());
    if (conn.ssid_esc) {
        send_str(req, "<dt>WiFi</dt><dd>");
        send_str(req, conn.ssid_esc);
        snprintf(buf, sizeof(buf), " (%d dBm%s)</dd>", conn.rssi, conn.wifi_weak ? ", weak" : "");
        send_str(req, buf);
    } else {
        send_kv(req, "WiFi", "Not connected");
    }
    if (conn.mqtt_enabled) {
        send_str(req, "<dt>MQTT</dt><dd>");
        send_str(req, conn.mqtt_connected ? "Connected" : "Disconnected");
        send_str(req, " (");
        send_str(req, conn.broker_esc);
        send_str(req, ")</dd>");
    } else {
        send_kv(req, "MQTT", "Disabled");
    }

    char serial[DEVICE_SERIAL_LEN];
    device_get_serial(serial, sizeof(serial));
    send_kv(req, "Serial", serial);
    send_kv(req, "Firmware", app_desc->version);
    send_kv(req, "Project", app_desc->project_name);
    snprintf(buf, sizeof(buf), "%s %s", app_desc->date, app_desc->time);
    send_kv(req, "Built", buf);
    snprintf(buf, sizeof(buf), "v%" PRIu32 " (%s)", firmware_update_bootloader_version(),
             firmware_update_bootloader_has_rollback()
                 ? "update rollback" : "no update rollback - flash over USB to get it");
    send_kv(req, "Bootloader", buf);
    format_uptime(buf, sizeof(buf));
    send_kv(req, "Uptime", buf);
    send_str(req, "</dl></section>"
                  "<section class='card'><h2><i class='i i-gauge' aria-hidden='true'></i>Bus and memory</h2><dl class='kv'>");

    send_str(req, DEVICE_PAGE_HEALTH);
    send_settings_toggle(req, "usageReport", "/usage_report",
                         "Share anonymous usage",
                         usage_report_get_enabled(), USAGE_REPORT_DESC);
    send_str(req, DEVICE_PAGE_TAIL);
    send_page_end(req);

    conn_status_free(&conn);
    return ESP_OK;
}

// ======================================================
// WiFi Config Page Handler
// ======================================================

// HTML page for WiFi provisioning
static const char WIFI_PAGE_HEAD[] =
    "<div class='page-head'><h1>WiFi</h1></div>"
    "<p class='lead'>Choose the network this device joins. Connecting restarts the device.</p>"
    "<div class='chips'>";

// Shown under the chip when the current signal is weak
static const char WIFI_WEAK_NOTE[] =
    "<div role='alert' data-variant='warning' class='narrow mb-4'>"
    "Weak signal. Below " STR(WIFI_RSSI_WEAK_DBM) " dBm the connection can drop out; moving the device "
    "or the access point closer, or adding a WiFi extender, should help.</div>";

// Follows the <form> opening tag, which the handler sends with the mDNS host
static const char WIFI_PAGE_FORM[] =
    "<div data-field>"
        "<label for='ssid'>Network</label>"
        "<select id='ssid' name='ssid' required>"
            "<option value=''>Scanning...</option>"
        "</select>"
    "</div>"
    "<div data-field>"
        "<label for='password'>Password</label>"
        "<input type='password' id='password' name='password' placeholder='Network password' maxlength='64' required>"
    "</div>"
    "<div class='toolbar'>"
    "<button type='submit'>Connect</button>"
    "<button type='button' id='retryScan' class='outline' hidden onclick='scanWiFi()'>Scan again</button>"
    "</div>"
    "<div id='status' role='alert' hidden></div>"
    "</form>"
    "<script>"
    "function showStatus(msg,isError){showAlert(document.getElementById('status'),msg,isError?'danger':'success');}"
    // /scan answers 202 while the scan is still running; poll until the list arrives.
    "function scanWiFi(){"
        "const s=document.getElementById('ssid');"
        "s.innerHTML='<option value=\"\">Scanning...</option>';"
        "document.getElementById('retryScan').hidden=true;"
        "const poll=()=>fetch('/scan')"
        ".then(r=>{if(r.status===202)return new Promise(ok=>setTimeout(ok," STR(WIFI_SCAN_POLL_MS) ")).then(poll);"
        "if(!r.ok)throw new Error('Scan failed ('+r.status+')');return r.json();});"
        "poll().then(data=>{"
            "s.innerHTML='';"
            "data.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;"
            "o.text=n.ssid+' ('+n.rssi+' dBm'+(n.rssi<" STR(WIFI_RSSI_WEAK_DBM) "?', weak':'')+')'+(n.current?' \\u2713':'');"
            "if(n.current)o.selected=true;s.appendChild(o);});})"
        ".catch(e=>{"
            "s.innerHTML='<option value=\"\">No networks found</option>';"
            "showStatus(e.message,true);"
            "document.getElementById('retryScan').hidden=false;});}"
    "document.getElementById('wifiForm').onsubmit=function(e){"
        "e.preventDefault();const ssid=document.getElementById('ssid').value;"
        "const pass=document.getElementById('password').value;"
        "if(!confirmRestart('Connect to '+ssid+'?'))return false;"
        "const st=document.getElementById('status');showAlert(st,'Saving\\u2026');"
        // The device comes back on the chosen network, likely at a new
        // address, so open it by its mDNS name rather than reloading here.
        "const host=this.dataset.host;"
        "fetch('/provision',{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({ssid:ssid,password:pass})}).then(r=>r.json()).then(data=>{"
        "if(data.success){restartReload(st,'Saved.',host?'http://'+host+'.local/':'');}"
        "else{showStatus('Failed: '+data.message,true);}}).catch(e=>{"
        "showStatus('Error: '+e,true);});return false;};"
    "scanWiFi();"
    "</script>";

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    conn_status_t conn;
    conn_status_load(&conn);
    send_page_start(req, "WiFi", PAGE_WIFI);
    send_str(req, WIFI_PAGE_HEAD);
    send_wifi_chip(req, &conn, NULL);
    send_str(req, "</div>");
    send_str(req, conn.wifi_weak ? WIFI_WEAK_NOTE : NULL);
    send_str(req, "<form id='wifiForm' class='card narrow' data-host='");
    send_str(req, wifi_get_mdns_hostname());   // generated name: no escaping needed
    send_str(req, "'>");
    send_str(req, WIFI_PAGE_FORM);
    send_page_end(req);
    conn_status_free(&conn);
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
        send_json(req, "[]");
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

    send_json(req, json_resp);

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
    send_json(req, "{\"scanning\":true}");
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
    char password[65] = {0};  // Up to a 64-hex-digit raw WPA2 key

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
        send_json(req, "{\"success\":false,\"message\":\"Invalid SSID\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Received WiFi credentials: SSID=%s", ssid);

    // Save credentials to NVS
    esp_err_t err = wifi_credentials_save(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials to NVS: %s", esp_err_to_name(err));
        send_json(req, "{\"success\":false,\"message\":\"Failed to save credentials\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Credentials saved to NVS successfully");

    // Send success response
    send_json(req, "{\"success\":true,\"message\":\"Saved\"}");

    // Restart device to apply new credentials
    vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
    esp_restart();

    return ESP_OK;
}

// ======================================================
// Pool Status Handler
// ======================================================

static const seen_device_t *find_seen_device(const pool_state_t *st, uint8_t hi, uint8_t lo)
{
    for (int i = 0; i < st->num_seen_devices; i++) {
        if (st->seen_devices[i].addr_hi == hi && st->seen_devices[i].addr_lo == lo) {
            return &st->seen_devices[i];
        }
    }
    return NULL;
}

// Adds "major.minor" under `key`, or null when the device (or its CMD 0x0A) hasn't been seen
static void add_fw_version(cJSON *obj, const char *key, const seen_device_t *d)
{
    if (d && d->fw_version_valid) {
        char fw_str[16];
        snprintf(fw_str, sizeof(fw_str), "%d.%d", d->fw_version_major, d->fw_version_minor);
        cJSON_AddStringToObject(obj, key, fw_str);
    } else {
        cJSON_AddNullToObject(obj, key);
    }
}

// Add one temperature sensor's reading (°C, or null when not connected) and
// its connected flag.
static void add_temp_reading(cJSON *obj, const char *key, const char *connected_key,
                             uint8_t value, bool connected)
{
    if (connected) {
        cJSON_AddNumberToObject(obj, key, value);
    } else {
        cJSON_AddNullToObject(obj, key);
    }
    cJSON_AddBoolToObject(obj, connected_key, connected);
}

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
        add_fw_version(dev, "firmware_version", d);

        // Temperatures hosted by this device — only emitted for devices that
        // have broadcast a CMD 0x16 reading. Each sensor has a value (°C,
        // whatever the display scale; null when not connected) and a
        // `_connected` flag. `temperature2` only appears for multi-sensor
        // sources (LEN 0x0E payloads).
        if (d->has_temp_sensors) {
            add_temp_reading(dev, "temperature1", "temperature1_connected",
                             d->temp1, d->temp1_connected);
            if (!d->single_sensor_source) {
                add_temp_reading(dev, "temperature2", "temperature2_connected",
                                 d->temp2, d->temp2_connected);
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

    // Heaters (configured heaters that have on/off state and/or setpoints)
    cJSON *heaters_arr = cJSON_CreateArray();
    for (int i = 0; i < MAX_HEATERS; i++) {
        bool config_known;
        if (!heater_configured(&state, i, &config_known)) {
            continue;
        }
        if (!state.heaters[i].valid && !heater_setpoint_valid(&state.heaters[i])) {
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
        if (heater_setpoint_valid(&state.heaters[i])) {
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

    // Touchscreen's "Use separate pool and spa modes" setting (register 0x64)
    if (state.separate_pool_spa_modes_valid) {
        cJSON_AddBoolToObject(root, "separate_pool_spa_modes", state.separate_pool_spa_modes);
    } else {
        cJSON_AddNullToObject(root, "separate_pool_spa_modes");
    }

    // Number of heaters configured on the Touchscreen (registers 0xC8, 0xCC);
    // null until every heater's configuration register has been received
    int heaters_configured = 0;
    bool heaters_known = true;
    for (int i = 0; i < MAX_HEATERS; i++) {
        bool known;
        if (heater_configured(&state, i, &known)) {
            heaters_configured++;
        }
        heaters_known = heaters_known && known;
    }
    if (heaters_known) {
        cJSON_AddNumberToObject(root, "heaters_configured", heaters_configured);
    } else {
        cJSON_AddNullToObject(root, "heaters_configured");
    }

    // Whether a Solar or Solar Controller channel is configured; null until
    // any channel configuration has been received
    bool any_channel = false, has_solar = false;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!state.channels[i].configured) continue;
        any_channel = true;
        if (state.channels[i].type == CHANNEL_TYPE_SOLAR ||
            state.channels[i].type == CHANNEL_TYPE_SOLAR_CONTROLLER) {
            has_solar = true;
        }
    }
    if (any_channel) {
        cJSON_AddBoolToObject(root, "has_solar", has_solar);
    } else {
        cJSON_AddNullToObject(root, "has_solar");
    }

    // Station signal strength; null while not connected
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
    } else {
        cJSON_AddNullToObject(root, "wifi_rssi");
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
            state.channels[i].type == CHANNEL_TYPE_SECONDARY_HEATER ||
            state.channels[i].type == CHANNEL_TYPE_LIGHT_ZONE) {
            continue;
        }
        const char *type_name  = get_channel_type_name(state.channels[i].type);
        const char *state_name = channel_state_name(state.channels[i].state);
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddNumberToObject(ch, "id",    state.channels[i].id);
        cJSON_AddStringToObject(ch, "name",  state.channels[i].name[0] ? state.channels[i].name : type_name);
        cJSON_AddStringToObject(ch, "type",  type_name);
        cJSON_AddStringToObject(ch, "state", state_name);
        cJSON_AddBoolToObject(ch, "active", state.channels[i].active);
        cJSON_AddItemToArray(channels, ch);
    }
    cJSON_AddItemToObject(root, "channels", channels);

    // Lighting
    cJSON *lighting = cJSON_CreateArray();
    for (int i = 0; i < MAX_LIGHT_ZONES; i++) {
        if (!state.lighting[i].configured) continue;
        const char *state_name = off_auto_on_name(state.lighting[i].state);
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

        const char *state_name = off_auto_on_name(state.valves[i].state);
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
    if (state.ph_setpoint_valid) {
        cJSON_AddNumberToObject(chlorinator, "ph_setpoint", state.ph_setpoint / 10.0);
    } else {
        cJSON_AddNullToObject(chlorinator, "ph_setpoint");
    }
    if (state.ph_valid) {
        cJSON_AddNumberToObject(chlorinator, "ph_reading", state.ph_reading / 10.0);
    } else {
        cJSON_AddNullToObject(chlorinator, "ph_reading");
    }
    if (state.orp_setpoint_valid) {
        cJSON_AddNumberToObject(chlorinator, "orp_setpoint", state.orp_setpoint);
    } else {
        cJSON_AddNullToObject(chlorinator, "orp_setpoint");
    }
    if (state.orp_valid) {
        cJSON_AddNumberToObject(chlorinator, "orp_reading", state.orp_reading);
    } else {
        cJSON_AddNullToObject(chlorinator, "orp_reading");
    }
    if (state.chlor_output_level_valid) {
        cJSON_AddNumberToObject(chlorinator, "chlor_output_level", state.chlor_output_level);
    } else {
        cJSON_AddNullToObject(chlorinator, "chlor_output_level");
    }
    // Chlorinator statistics, under the same keys as the MQTT chlorinator state
    for (int i = 0; i < CHLOR_STAT_COUNT; i++) {
        const char *key = mqtt_chlor_stat_key((chlor_stat_t)i);
        if (!state.chlor_stats_valid[i]) {
            cJSON_AddNullToObject(chlorinator, key);
        } else if (chlor_stat_is_ph((chlor_stat_t)i)) {
            cJSON_AddNumberToObject(chlorinator, key, state.chlor_stats[i] / 10.0);
        } else {
            cJSON_AddNumberToObject(chlorinator, key, state.chlor_stats[i]);
        }
    }
    cJSON_AddItemToObject(root, "chlorinator", chlorinator);

    // Internet Gateway
    cJSON *gateway = cJSON_CreateObject();
    if (state.serial_number_valid) {
        cJSON_AddNumberToObject(gateway, "serial_number", (double)state.serial_number);
    } else {
        cJSON_AddNullToObject(gateway, "serial_number");
    }
    add_fw_version(gateway, "firmware_version", find_seen_device(&state, 0x00, 0xF0));
    static const char *const GATEWAY_IFACE_KEYS[MAX_GATEWAY_IFACES] = {
        [GATEWAY_IFACE_WIRED] = "wired",
        [GATEWAY_IFACE_WIFI]  = "wifi",
    };
    for (int i = 0; i < MAX_GATEWAY_IFACES; i++) {
        const gateway_iface_t *gw = &state.gateway_ifaces[i];
        if (gw->valid) {
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
                     gw->ip[0], gw->ip[1], gw->ip[2], gw->ip[3]);
            cJSON *iface = cJSON_CreateObject();
            cJSON_AddStringToObject(iface, "ip",           ip_str);
            cJSON_AddNumberToObject(iface, "signal_level", gw->signal_level);
            cJSON_AddBoolToObject(iface,   "enabled",      gw->enabled);
            cJSON_AddBoolToObject(iface,   "initialized",  gw->initialized);
            cJSON_AddBoolToObject(iface,   "linked",       gw->linked);
            cJSON_AddItemToObject(gateway, GATEWAY_IFACE_KEYS[i], iface);
        } else {
            cJSON_AddNullToObject(gateway, GATEWAY_IFACE_KEYS[i]);
        }
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
    add_fw_version(touchscreen, "version", find_seen_device(&state, 0x00, 0x50));
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

    send_json(req, json_resp);

    cJSON_free(json_resp);
    return ESP_OK;
}

// ======================================================
// Status JSON Page Handler
// ======================================================

static const char STATUS_VIEW_PAGE[] =
    "<div class='page-head'><h1>Status JSON</h1>"
    "<div class='actions'>"
    "<button type='button' id='copy-btn' class='outline'><i class='i i-copy' aria-hidden='true'></i><span>Copy</span></button>"
    "<a href='/status' id='dl-json' class='button outline' download><i class='i i-download' aria-hidden='true'></i>Download</a>"
    "</div></div>"
    "<p id='time-info' class='lead' hidden></p>"
    "<div id='status-error' role='alert' data-variant='danger' hidden></div>"
    "<pre class='json'><code id='status-content'>Loading status...</code></pre>"
    "<script>"
    "bindJsonDownload('dl-json','pool-controller-status');"
    "fetch('/status').then(r=>r.json()).then(data=>{"
    "document.getElementById('status-content').textContent=JSON.stringify(data,null,2);"
    "let localTime='NTP not synced';"
    "if(data.current_time&&data.current_time!=='NTP not synced'){"
    "localTime=fmtDateTime(new Date(data.current_time.replace(' UTC','Z').replace(' ','T')));}"
    "const ti=document.getElementById('time-info');"
    "ti.textContent='Device time '+localTime+' \u00b7 last bus update '+data.time_since_last_update;"
    "ti.hidden=false;"
    "}).catch(e=>{"
    "showAlert(document.getElementById('status-error'),'Error loading status: '+e,'danger');"
    "document.getElementById('status-content').textContent='';"
    "});"
    "document.getElementById('copy-btn').addEventListener('click',function(){"
    // navigator.clipboard needs a secure context, which a plain-http
    // device page isn't, so copy through a selection instead.
    "const sel=getSelection(),rg=document.createRange();"
    "rg.selectNodeContents(document.getElementById('status-content'));"
    "sel.removeAllRanges();sel.addRange(rg);const ok=document.execCommand('copy');sel.removeAllRanges();"
    "const b=this.querySelector('span');b.textContent=ok?'Copied':'Copy failed';setTimeout(()=>{b.textContent='Copy';},1500);});"
    "</script>";

static esp_err_t status_view_get_handler(httpd_req_t *req)
{
    return send_static_page(req, "Status JSON", PAGE_STATUS, STATUS_VIEW_PAGE);
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
        "<div class='page-head'><h1>MQTT</h1></div>"
        "<p class='lead'>Publish pool state to an MQTT broker, with Home Assistant discovery. "
        "Saving restarts the device.</p>"
        "<form id='mqttForm' class='card narrow'>";

    char *broker_esc   = html_escape(config.broker);
    char *username_esc = html_escape(config.username);
    const char *broker_val   = broker_esc   ? broker_esc   : "";
    const char *username_val = username_esc ? username_esc : "";
    const char *pw_placeholder = strlen(config.password) > 0
        ? "Leave empty to keep current password" : "Leave empty if not required";
    static const char html_fields_fmt[] =
        "<div data-field>"
            "<label class='switch-row'><input type='checkbox' id='enabled' role='switch' name='enabled'%s> Enable MQTT</label>"
        "</div>"
        "<div data-field>"
            "<label for='broker'>Broker host or IP</label>"
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
        "<button type='submit'>Save</button>"
        "<div id='status' role='alert' hidden class='mt-4'></div>"
        "</form>"
        "<script>"
        "document.getElementById('mqttForm').addEventListener('submit',function(e){"
        "e.preventDefault();"
        "if(!confirmRestart('Save the MQTT settings?'))return;"
        "const data={enabled:document.getElementById('enabled').checked,"
        "broker:document.getElementById('broker').value,"
        "port:parseInt(document.getElementById('port').value)||1883,"
        "username:document.getElementById('username').value,"
        "password:document.getElementById('password').value};"
        "fetch('/mqtt_config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})"
        ".then(r=>r.json()).then(d=>{"
        "const s=document.getElementById('status');"
        "if(d.success)restartReload(s,'Saved.');else showAlert(s,d.message,'danger');"
        "}).catch(()=>{"
        "showAlert(document.getElementById('status'),'Failed to save configuration','danger');});"
        "});"
        "</script>";

    send_page_start(req, "MQTT", PAGE_MQTT);
    send_str(req, html_start);
    send_str(req, html_fields);
    send_str(req, html_end);
    send_page_end(req);
    free(html_fields);

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
        send_json(req, "{\"success\":false,\"message\":\"Failed to save config\"}");
        return ESP_OK;
    }

    // Send success response
    send_json(req, "{\"success\":true,\"message\":\"Saved\"}");

    // Restart device to apply new MQTT config
    vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
    esp_restart();

    return ESP_OK;
}

// ======================================================
// OTA Update Handlers
// ======================================================

// Send one settings switch with its description, a status box (<id>Status)
// and the bindToggle() call that POSTs <url>?enabled=0|1 on change. Sent as
// fixed chunks around the " checked" attribute, so there is no buffer to size
// and nothing to truncate. The arguments are inserted unescaped, so pass only
// fixed strings. desc_html is a <p> shown under the switch.
static void send_settings_toggle(httpd_req_t *req, const char *id, const char *url,
                                 const char *label, bool checked, const char *desc_html)
{
    const char *parts[] = {
        "<label class='switch-row'><input type='checkbox' role='switch' id='", id, "'",
        checked ? " checked" : "", "> ", label, "</label>",
        desc_html,
        "<div id='", id, "Status' role='alert' hidden class='mt-2'></div>"
        "<script>bindToggle('", id, "','", url, "');</script>",
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        send_str(req, parts[i]);
    }
}

static const char FIRMWARE_PAGE_GITHUB[] =
    "</dl></section>"

    // ---- Update from GitHub -------------------------------------------
    "<section class='card'><h2><i class='i i-download' aria-hidden='true'></i>Update from GitHub</h2>"
    "<div id='ghStatus' role='alert'>Checking for the latest release&hellip;</div>"
    "<progress id='ghProgress' value='0' max='100' hidden class='mt-2'></progress>"
    "<div class='toolbar mt-4'>"
    "<select id='ghVersion' hidden aria-label='Version to install' style='flex:1;min-width:10rem;margin:0'></select>"
    "<button type='button' id='ghInstall' hidden>Install</button>"
    "<button type='button' id='ghCheck' class='outline'>Check now</button>"
    "</div>"
    "<script>"
    "(function(){"
    "const s=document.getElementById('ghStatus');"
    "const p=document.getElementById('ghProgress');"
    "const vs=document.getElementById('ghVersion');"
    "const bC=document.getElementById('ghCheck');"
    "const bI=document.getElementById('ghInstall');"
    "let polling=false;let last='';let rebooting=false;"
    // Populate the version dropdown (newest first) only when the list
    // changes, so an in-progress user selection isn't reset by polling.
    // "(latest)" marks the release offered as the update, which skips
    // release candidates (unless one is installed), so it need not be the
    // first entry.
    "function fill(list,pre,latest){"
    "const sig=(list||[]).join(',')+'|'+latest;"
    "if(vs.dataset.sig===sig)return;"
    "vs.dataset.sig=sig;vs.innerHTML='';"
    "(list||[]).forEach(function(v,i){"
    "const o=document.createElement('option');"
    "const t=[];if(v===latest)t.push('latest');if(pre&&pre[i])t.push('pre-release');"
    "o.value=v;o.textContent=v+(t.length?' ('+t.join(', ')+')':'');"
    "if(v===latest)o.selected=true;vs.appendChild(o);});}"
    "function render(d){"
    "if(d.state==='downloading'){"
    "p.hidden=false;p.value=d.progress||0;"
    "showAlert(s,'Downloading update '+(d.progress||0)+'%… do not power off.');"
    "vs.hidden=bC.hidden=bI.hidden=true;return;}"
    "if(d.state==='success'){p.hidden=false;p.value=100;"
    "vs.hidden=bC.hidden=bI.hidden=true;"
    "if(!rebooting){rebooting=true;restartReload(s,'Update installed.');}return;}"
    "p.hidden=true;"
    // Hidden while a check runs, so a second click can't start a second poll loop.
    "bC.hidden=d.state==='checking';"
    "const have=!!(d.versions&&d.versions.length);"
    "if(have)fill(d.versions,d.prerelease,d.latest);"
    "vs.hidden=bI.hidden=!have;"
    "if(d.state==='checking'){showAlert(s,'Checking for the latest release…');}"
    "else if(d.state==='error'&&d.error){showAlert(s,'Error: '+d.error,'danger');}"
    "else if(d.update_available){"
    "showAlert(s,'Update available: '+d.latest+' (installed '+d.installed+'). ','warning');"
    "if(d.release_url){const a=document.createElement('a');a.href=d.release_url;a.target='_blank';"
    "a.textContent='Release notes';s.appendChild(a);}}"
    "else if(d.checked){showAlert(s,'Up to date ('+d.installed+'). You can reinstall or roll back below.','success');}"
    "else{showAlert(s,'Latest release not yet checked.');}}"
    // q='?refresh=1' asks the device to start a check; the reply then reads
    // 'checking' and polling carries on until the result arrives.
    "function poll(q){fetch('/update/check'+(q||'')).then(r=>r.json()).then(function(d){"
    "last=d.state;render(d);"
    "if(d.state==='downloading'||d.state==='checking'){polling=true;setTimeout(function(){poll();},2000);}"
    "else{polling=false;}"
    // A failed poll right after a download means the device rebooted
    // before we saw the 'success' state — treat it as success.
    "}).catch(function(){polling=false;"
    "if(last==='downloading')render({state:'success'});"
    "else if(q){showAlert(s,'Check failed.','danger');}});}"
    "bC.addEventListener('click',function(){"
    "if(polling)return;polling=true;"
    "showAlert(s,'Checking for the latest release…');"
    "poll('?refresh=1');});"
    "bI.addEventListener('click',function(){"
    "const v=vs.value;if(!v)return;"
    "if(!confirmRestart('Download and install firmware '+v+' from GitHub?'))return;"
    "fetch('/update/github?version='+encodeURIComponent(v),{method:'POST'}).then(r=>r.json()).then(function(d){"
    "if(d.success){if(!polling)poll();}"
    "else{showAlert(s,'Error: '+(d.message||'could not start'),'danger');}});});"
    "poll();"
    "})();"
    "</script>"
    "<hr>";

static const char FIRMWARE_AUTO_CHECK_DESC[] =
    "<p>Checks GitHub every 12 hours. You can still check by hand when this is off.</p>";

static const char FIRMWARE_PAGE_TAIL[] =
    "</section>"

    // ---- Manual upload -------------------------------------------------
    "<section class='card'><h2><i class='i i-upload' aria-hidden='true'></i>Upload a file</h2>"
    "<form id='updateForm'>"
    "<label class='dropzone' id='dropzone'>"
    "<input type='file' id='firmware' name='firmware' accept='.bin' required>"
    "<i class='i i-upload' aria-hidden='true'></i><span id='dropText'>Drop a firmware .bin here or choose a file</span>"
    "</label>"
    "<div class='toolbar mt-4'><button type='submit'>Upload and update</button></div>"
    "<div id='status' role='alert' hidden></div>"
    "<progress id='progressBar' value='0' max='100' hidden class='mt-4'></progress>"
    "<p id='progressText' hidden class='text-center mt-2 small muted'></p>"
    "</form>"
    "</section>"
    "<p class='small muted'><i class='i i-alert' aria-hidden='true'></i> "
    "Installing an update restarts the device. Keep it powered until this page reloads.</p>"
    "</div>"
    "<script>"
    "(function(){"
    "const input=document.getElementById('firmware');"
    "const dz=document.getElementById('dropzone');"
    "input.addEventListener('change',function(){"
    "document.getElementById('dropText').textContent=input.files[0]?input.files[0].name:"
    "'Drop a firmware .bin here or choose a file';});"
    "['dragenter','dragover'].forEach(t=>dz.addEventListener(t,()=>dz.classList.add('over')));"
    "['dragleave','drop'].forEach(t=>dz.addEventListener(t,()=>dz.classList.remove('over')));"
    "})();"
    "document.getElementById('updateForm').addEventListener('submit',function(e){"
    "e.preventDefault();"
    "const file=document.getElementById('firmware').files[0];"
    "if(!file){alert('Please select a file');return;}"
    "if(!confirmRestart('Install '+file.name+'?'))return;"
    "const status=document.getElementById('status');"
    "const progressBar=document.getElementById('progressBar');"
    "const progressText=document.getElementById('progressText');"
    "showAlert(status,'Uploading firmware ('+Math.round(file.size/1024)+'KB)...');"
    "progressBar.value=0;"
    "progressBar.hidden=false;"
    "progressText.hidden=false;"
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
    "progressBar.hidden=true;progressText.hidden=true;"
    "restartReload(status,'Update installed.');"
    "}else{"
    "showAlert(status,'Error: '+resp.message,'danger');"
    "}}else{"
    "showAlert(status,'Upload failed (HTTP '+xhr.status+')','danger');"
    "}});"
    "xhr.addEventListener('error',function(){"
    "showAlert(status,'Network error during upload','danger');});"
    "xhr.addEventListener('timeout',function(){"
    "showAlert(status,'Upload timeout - try again','danger');});"
    "xhr.open('POST','/update',true);"
    "xhr.timeout=120000;"
    "xhr.setRequestHeader('Content-Type','application/octet-stream');"
    "xhr.send(file);"
    "});"
    "</script>";

static esp_err_t update_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    send_page_start(req, "Firmware", PAGE_FIRMWARE);
    send_str(req, "<div class='page-head'><h1>Firmware</h1></div>"
                  "<div class='stack' style='max-width:44rem'>"
                  "<section class='card'><h2>Installed</h2><dl class='kv'>");
    send_kv(req, "Version", app_desc->version);
    send_kv(req, "Running from", running->label);
    send_kv(req, "Next update to", update_partition ? update_partition->label : "unknown");
    send_str(req, FIRMWARE_PAGE_GITHUB);
    send_settings_toggle(req, "autoCheck", "/update/auto_check",
                         "Check for new releases automatically",
                         firmware_update_get_auto_check(), FIRMWARE_AUTO_CHECK_DESC);
    send_str(req, FIRMWARE_PAGE_TAIL);
    send_page_end(req);
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
        send_json(req, "{\"success\":false,\"message\":\"No OTA partition configured\"}");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%lx", update_partition->label, update_partition->address);

    // Start OTA update
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        send_json(req, "{\"success\":false,\"message\":\"OTA begin failed\"}");
        return ESP_FAIL;
    }

    // Read and write firmware data
    const size_t buf_size = HTTP_OTA_BUFFER_SIZE;
    char *buf = malloc(buf_size);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        send_json(req, "{\"success\":false,\"message\":\"Memory allocation failed\"}");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > OTA_MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Invalid firmware size: %d (must be 1–%d bytes)", remaining, OTA_MAX_FIRMWARE_SIZE);
        free(buf);
        esp_ota_abort(ota_handle);
        send_json(req, "{\"success\":false,\"message\":\"Invalid firmware size\"}");
        return ESP_FAIL;
    }

    int received = 0;
    ESP_LOGI(TAG, "Firmware size: %d bytes", remaining);

    // Socket timeouts are retried, but only until the upload deadline, so a
    // stalled client can't hold the (single) HTTP server task forever
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)OTA_UPLOAD_TIMEOUT_MS * 1000;

    while (remaining > 0) {
        size_t recv_size = MIN(remaining, buf_size);
        int recv_len = httpd_req_recv(req, buf, recv_size);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < deadline_us) {
            continue;
        }
        // 0 means the client closed the connection mid-upload
        if (recv_len <= 0) {
            ESP_LOGE(TAG, "Firmware receive failed (%d) after %d/%d bytes",
                     recv_len, received, req->content_len);
            free(buf);
            esp_ota_abort(ota_handle);
            send_json(req, "{\"success\":false,\"message\":\"Receive failed\"}");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, (const void *)buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            send_json(req, "{\"success\":false,\"message\":\"OTA write failed\"}");
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

    free(buf);

    // Finish OTA update
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            send_json(req, "{\"success\":false,\"message\":\"Image validation failed\"}");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            send_json(req, "{\"success\":false,\"message\":\"OTA end failed\"}");
        }
        return ESP_FAIL;
    }

    // Set boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        send_json(req, "{\"success\":false,\"message\":\"Failed to set boot partition\"}");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful!");

    send_json(req, "{\"success\":true,\"message\":\"Update installed\"}");

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
// With ?refresh=1, first ask the background task to re-query GitHub, the same
// path the HA button takes. The reply then reads "checking", and the page
// polls until the result arrives, so the GitHub request never holds up the
// web server.
static esp_err_t update_check_handler(httpd_req_t *req)
{
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "refresh", val, sizeof(val)) == ESP_OK &&
            strcmp(val, "1") == 0) {
            firmware_update_request_check();
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

    // Recent releases, newest first, so the UI can offer installing or
    // rolling back to a specific version. prerelease[i] flags versions[i] as
    // a release candidate, offered as "latest" only to an installed one.
    cJSON *versions = cJSON_CreateArray();
    cJSON *prerelease = cJSON_CreateArray();
    for (int i = 0; i < st.version_count && i < FW_UPDATE_MAX_VERSIONS; i++) {
        cJSON_AddItemToArray(versions, cJSON_CreateString(st.versions[i]));
        cJSON_AddItemToArray(prerelease,
                             cJSON_CreateBool(firmware_version_is_prerelease(st.versions[i])));
    }
    cJSON_AddItemToObject(root, "versions", versions);
    cJSON_AddItemToObject(root, "prerelease", prerelease);

    char *json = cJSON_PrintUnformatted(root);
    send_json(req, json ? json : "{}");
    cJSON_free(json);
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
    if (err == ESP_OK) {
        send_json(req, "{\"success\":true,\"message\":\"Update started\"}");
    } else if (err == ESP_ERR_NOT_FOUND) {
        send_json(req, "{\"success\":false,\"message\":\"Unknown release version\"}");
    } else {
        send_json(req, "{\"success\":false,\"message\":\"No release available or an update is already running\"}");
    }
    return ESP_OK;
}

// Shared by the update page's settings checkboxes: POST <uri>?enabled=0|1.
// Applies the value with `set`, then replies with what `get` reports, so the
// page can show the setting now in force.
static esp_err_t toggle_post(httpd_req_t *req, void (*set)(bool), bool (*get)(void))
{
    char query[32];
    char val[4];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "enabled", val, sizeof(val)) != ESP_OK ||
        (strcmp(val, "0") != 0 && strcmp(val, "1") != 0)) {
        httpd_resp_set_status(req, "400 Bad Request");
        send_json(req, "{\"success\":false,\"message\":\"enabled must be 0 or 1\"}");
        return ESP_OK;
    }

    set(val[0] == '1');
    send_json(req, get() ? "{\"success\":true,\"enabled\":true}"
                         : "{\"success\":true,\"enabled\":false}");
    return ESP_OK;
}

// POST /update/auto_check?enabled=0|1 — turn the 12-hourly release check on or off.
static esp_err_t update_auto_check_post_handler(httpd_req_t *req)
{
    return toggle_post(req, firmware_update_set_auto_check, firmware_update_get_auto_check);
}

// Turning usage reporting off also has the server delete this install's rows;
// waking the check task sends that request now rather than at the next
// 12-hourly run.
static void set_usage_report(bool enabled)
{
    usage_report_set_enabled(enabled);
    if (!enabled) {
        firmware_update_wake();
    }
}

// POST /usage_report?enabled=0|1 — turn anonymous usage reporting on or off.
static esp_err_t usage_report_post_handler(httpd_req_t *req)
{
    return toggle_post(req, set_usage_report, usage_report_get_enabled);
}

// POST /reboot — restart the device.
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reboot requested via web UI");

    send_json(req, "{\"success\":true,\"message\":\"Restarting\"}");

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

    if (!mqtt_discovery_reset_start()) {
        send_json(req, "{\"success\":false,\"message\":\"MQTT is not connected\"}");
        return ESP_OK;
    }

    send_json(req, "{\"success\":true,\"message\":\"Resetting entities...\"}");
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
        send_json(req, "{\"success\":false,\"message\":\"Invalid request\"}");
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
                send_json(req, "{\"success\":false,\"message\":\"Invalid hex string\"}");
                return ESP_OK;
            }
        } else {
            break;
        }
    }

    if (msg_len == 0) {
        send_json(req, "{\"success\":false,\"message\":\"No bytes parsed\"}");
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

    send_json(req, json_resp);

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

    send_json(req, json);
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

    send_json(req, "{\"ok\":true}");
    return ESP_OK;
}

static const char UNKNOWN_MSGS_PAGE[] =
    "<div class='page-head'><h1>Unknown messages</h1>"
    "<div class='actions'>"
    "<a href='/unknown_msgs' id='dl-json' class='button outline' download><i class='i i-download' aria-hidden='true'></i>Download</a>"
    "<span class='sep' aria-hidden='true'></span>"
    "<button id='clear-btn' class='outline' data-variant='danger'><i class='i i-trash' aria-hidden='true'></i>Clear</button>"
    "</div></div>"
    "<p class='lead'><span id='unk-count'></span>Unrecognized and error frames captured from the bus, "
    "newest first (up to " STR(UNKNOWN_BUFFER_CAPACITY) " kept). Select a row to see the full frame.</p>"
    "<div id='unk-error' role='alert' data-variant='danger' hidden></div>"
    "<p id='unk-empty' hidden class='muted'>No unknown messages recorded yet.</p>"
    "<div id='unk-table' class='table-wrap' hidden>"
    "<table><colgroup><col style='width:26%'><col style='width:8%'><col><col style='width:8%'><col style='width:22%'></colgroup>"
    "<thead><tr><th>From &rarr; to</th><th>CMD</th><th>Payload</th><th class='num'>Hits</th><th>Last seen</th></tr></thead>"
    "<tbody id='unk-tbody'></tbody></table>"
    "</div>"
    "<script>"
    "bindJsonDownload('dl-json','pool-controller-unknown-msgs');"
    "function fmtTime(ts){return ts>0?fmtDateTime(new Date(ts*1000)):'Unknown';}"
    "function mkEl(tag,txt,cls){"
    "const el=document.createElement(tag);"
    "if(txt!==undefined)el.textContent=txt;"
    "if(cls)el.className=cls;"
    "return el;}"
    "function load(){"
    "fetch('/unknown_msgs').then(r=>r.json()).then(data=>{"
    "const count=data.count||0;"
    "document.getElementById('unk-count').textContent="
    "count+' unique frame'+(count!==1?'s':'')+'. ';"
    "const empty=document.getElementById('unk-empty');"
    "const tbl=document.getElementById('unk-table');"
    "const tbody=document.getElementById('unk-tbody');"
    "tbody.innerHTML='';"
    "empty.hidden=count!==0;tbl.hidden=count===0;"
    "const entries=[...(data.entries||[])];"
    "entries.sort((a,b)=>b.last_seen-a.last_seen);"
    "entries.forEach(e=>{"
    "const tr=mkEl('tr',undefined,'row-toggle'+(e.is_error?' err':''));"
    // From -> to: source name (+ reason tag) over both addresses
    "const who=mkEl('td');"
    "who.appendChild(document.createTextNode((e.src_name||e.src)+' \\u2192 '+(e.dst_name||e.dst)));"
    "if(e.reason!=='unhandled')who.appendChild(mkEl('span',e.reason,'tag'+(e.is_error?' err':'')));"
    "who.appendChild(mkEl('div',e.src+' \\u2192 '+e.dst,'mono muted'));"
    "tr.appendChild(who);"
    "tr.appendChild(mkEl('td',e.cmd,'mono'));"
    "tr.appendChild(mkEl('td',e.payload||'(empty)','mono'));"
    "tr.appendChild(mkEl('td',String(e.hits),'num'));"
    "tr.appendChild(mkEl('td',fmtTime(e.last_seen),'small muted'));"
    // Detail row: the full raw frame and first-seen time, shown on click
    "const det=mkEl('tr',undefined,'detail');det.hidden=true;"
    "const dtd=mkEl('td');dtd.colSpan=5;"
    "dtd.appendChild(document.createTextNode('Raw frame: '));"
    "dtd.appendChild(mkEl('code',e.raw||'(empty)'));"
    "if(e.raw_len>" STR(UNKNOWN_BUFFER_MAX_RAW_BYTES) ")"
    "dtd.appendChild(document.createTextNode(' (first " STR(UNKNOWN_BUFFER_MAX_RAW_BYTES) " bytes of '+e.raw_len+')'));"
    "dtd.appendChild(mkEl('div','First seen '+fmtTime(e.first_seen),'muted mt-2'));"
    "det.appendChild(dtd);"
    "tr.addEventListener('click',()=>{det.hidden=!det.hidden;});"
    "tbody.appendChild(tr);tbody.appendChild(det);});"
    "}).catch(err=>{"
    "showAlert(document.getElementById('unk-error'),'Error loading data: '+err,'danger');});}"
    "load();"
    "document.getElementById('clear-btn').addEventListener('click',function(){"
    "if(!confirm('Clear all unknown message records?'))return;"
    "fetch('/unknown_msgs/clear',{method:'POST'}).then(()=>load()).catch(err=>{"
    "showAlert(document.getElementById('unk-error'),'Clear failed: '+err,'danger');});});"
    "</script>";

static esp_err_t unknown_msgs_view_handler(httpd_req_t *req)
{
    return send_static_page(req, "Unknown messages", PAGE_UNKNOWN, UNKNOWN_MSGS_PAGE);
}

// ======================================================
// Debug Log Page Handler
// ======================================================
// Lines arrive over the /ws/log WebSocket (see web_log.c). The capture lives
// only in this page's memory and is downloaded as a Blob — nothing is stored
// on the device or in browser storage.

static esp_err_t log_view_handler(httpd_req_t *req)
{
    const char *page_content =
        "<div class='page-head'><h1>Debug log</h1>"
        "<div class='actions'>"
        "<button id='pause-btn' class='outline'><i id='pause-icon' class='i i-pause' aria-hidden='true'></i><span>Pause</span></button>"
        "<button id='dl-btn' class='outline'><i class='i i-download' aria-hidden='true'></i>Download</button>"
        "<span class='sep' aria-hidden='true'></span>"
        "<button id='clear-btn' class='outline' data-variant='danger'><i class='i i-trash' aria-hidden='true'></i>Clear</button>"
        "</div></div>"
        "<p class='lead'>Live firmware log, including an <code>RX MSG</code> line for every bus frame. "
        "The capture is kept in this page only, and is lost on reload. Download it to attach to a bug report. "
        "Hex frames entered below the log are sent to the bus as-is.</p>"
        "<div class='toolbar'>"
        "<input id='filter' type='search' placeholder='Filter, e.g. RX MSG'>"
        "<small class='muted'><span id='log-dot' class='dot' style='display:inline-block;margin-right:0.4rem'></span>"
        "<span id='log-status'>Connecting\u2026</span></small>"
        "</div>"
        // Per-level line colours (app.css .lE/.lW/...) key on the ESP-IDF
        // "X (time) TAG: ..." prefix; markers are gaps the page inserted or
        // the device reported.
        "<pre id='log' class='log'></pre>"
        "<form id='tx-form' class='toolbar mt-2'>"
        "<input id='tx-hex' type='text' class='mono' autocomplete='off' spellcheck='false' "
        "placeholder='Send hex to bus, e.g. 02 00 50 FF FF 03'>"
        "<button id='tx-btn' type='submit' class='outline' disabled>Send</button>"
        "</form>"
        "<script>"
        "const CAP=" STR(WEB_LOG_BROWSER_MAX_LINES) ",SHOW=" STR(WEB_LOG_DISPLAY_MAX_LINES) ";"
        "const lines=[];let bytes=0,trimmed=0,partial='',paused=false,filter='',ws=null,everOpen=false,state='Connecting…';"
        "let captureStart=new Date(),gaps=0,reconnects=0;"
        "const logEl=document.getElementById('log'),statEl=document.getElementById('log-status');"
        "function matches(l){return !filter||l.toLowerCase().includes(filter);}"
        "function fmtSize(b){return b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB';}"
        "function status(){"
        "let s=state+' · '+lines.length+' lines · '+fmtSize(bytes);"
        "if(trimmed)s+=' · '+trimmed+' oldest lines discarded (page keeps '+CAP+')';"
        "if(paused)s+=' · paused';"
        "statEl.textContent=s;}"
        "function lineEl(l){"
        "const s=document.createElement('span'),m=/^([EWIDV]) \\(/.exec(l);"
        "if(m)s.className='l'+m[1];else if(l.startsWith('--- '))s.className='lM';"
        "s.textContent=l+'\\n';return s;}"
        "function render(){"
        "const out=[];"
        "for(let i=lines.length-1;i>=0&&out.length<SHOW;i--){if(matches(lines[i]))out.push(lines[i]);}"
        "const f=document.createDocumentFragment();"
        "for(let i=out.length-1;i>=0;i--)f.appendChild(lineEl(out[i]));"
        "logEl.replaceChildren(f);"
        "logEl.scrollTop=logEl.scrollHeight;}"
        "function add(newLines){"
        "for(const l of newLines){lines.push(l);bytes+=l.length+1;"
        "if(/^--- \\d+ lines dropped ---$/.test(l))gaps++;else if(l==='--- reconnected ---')reconnects++;}"
        "if(lines.length>CAP+1000){const n=lines.length-CAP;"
        "for(let i=0;i<n;i++)bytes-=lines[i].length+1;lines.splice(0,n);trimmed+=n;}"
        "if(!paused){"
        "const show=newLines.filter(matches);"
        "if(show.length){"
        "const atEnd=logEl.scrollHeight-logEl.scrollTop-logEl.clientHeight<40;"
        "const f=document.createDocumentFragment();"
        "for(const l of show)f.appendChild(lineEl(l));"
        "logEl.appendChild(f);"
        // Keep the rendered view bounded; when the user has scrolled up, shift
        // scrollTop by the height removed so the text they're reading stays put
        "if(logEl.childNodes.length>SHOW){const h=logEl.scrollHeight;"
        "while(logEl.childNodes.length>SHOW)logEl.firstChild.remove();"
        "if(!atEnd)logEl.scrollTop-=h-logEl.scrollHeight;}"
        "if(atEnd)logEl.scrollTop=logEl.scrollHeight;}}"
        "status();}"
        "function connect(){"
        "ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws/log');"
        "ws.onopen=()=>{if(everOpen)add(['--- reconnected ---']);everOpen=true;state='Live';txBtn.disabled=false;"
        "document.getElementById('log-dot').className='dot ok';status();};"
        "ws.onmessage=e=>{const parts=(partial+e.data).split('\\n');partial=parts.pop();"
        "add(parts.map(l=>l.replace(/\\r$/,'')));};"
        "ws.onclose=()=>{state='Disconnected, retrying…';txBtn.disabled=true;"
        "document.getElementById('log-dot').className='dot bad';status();ws=null;setTimeout(connect,2000);};"
        "ws.onerror=()=>{try{ws.close();}catch(_){}};}"
        "setInterval(()=>{if(ws&&ws.readyState===1)ws.send('k');}," STR(WEB_LOG_KEEPALIVE_MS) ");"
        // The device logs the send (and any parse/collision error); the frame's
        // echo then comes back decoded as an RX MSG line. The text is left in
        // the box so the same frame can be resent.
        "const txBtn=document.getElementById('tx-btn'),txHex=document.getElementById('tx-hex');"
        "document.getElementById('tx-form').addEventListener('submit',function(e){"
        "e.preventDefault();const v=txHex.value.trim();"
        "const ok=/^[0-9a-f]{2}(\\s*[0-9a-f]{2})*$/i.test(v);"
        "if(ok)txHex.removeAttribute('aria-invalid');else txHex.setAttribute('aria-invalid','true');"
        "if(ok&&ws&&ws.readyState===1)ws.send('" WEB_LOG_TX_PREFIX "'+v);});"
        "document.getElementById('pause-btn').addEventListener('click',function(){"
        "paused=!paused;this.querySelector('span').textContent=paused?'Resume':'Pause';"
        "document.getElementById('pause-icon').className='i i-'+(paused?'play':'pause');"
        "if(!paused)render();status();});"
        "document.getElementById('clear-btn').addEventListener('click',function(){"
        "if(lines.length&&!confirm('Discard the '+lines.length+' captured lines?'))return;"
        "lines.length=0;bytes=0;trimmed=0;gaps=0;reconnects=0;captureStart=new Date();render();status();});"
        "document.getElementById('filter').addEventListener('input',function(){"
        "filter=this.value.toLowerCase();render();});"
        // Download header: a '#'-commented summary from /status, fetched at
        // download time so it includes devices learned during the capture.
        // It sits above the log because the decoder replay test skips lines
        // before the first RX MSG but treats lines after one as its output.
        // SSID, IP addresses and serial numbers are left out on purpose, as
        // the file is meant to be attached to public bug reports.
        "function fmtLocal(d){return d.getFullYear()+'-'+pad2(d.getMonth()+1)+'-'+pad2(d.getDate())+' '"
        "+pad2(d.getHours())+':'+pad2(d.getMinutes())+':'+pad2(d.getSeconds());}"
        // A UTC offset rather than the IANA zone name, which names a city.
        "function fmtOff(d){const o=-d.getTimezoneOffset(),a=Math.abs(o);"
        "return'UTC'+(o<0?'-':'+')+pad2(Math.floor(a/60))+':'+pad2(a%60);}"
        "function fmtDur(ms){const s=Math.floor(ms/1000),d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);"
        "return(d?d+'d ':'')+(d||h?h+'h ':'')+m+'m';}"
        "function known(v,f){return v===null||v===undefined?'unknown':f(v);}"
        "const yn=v=>v?'yes':'no';"
        "function header(st,err,unk,end){"
        "const h=['Pool Controller debug log'];"
        "h.push('Captured:  '+fmtLocal(captureStart)+' to '+fmtLocal(end)+' ('+fmtOff(end)+')');"
        "h.push('Lines:     '+lines.length+' ('+trimmed+' oldest discarded by the page, '+gaps+' device gaps, '+reconnects+' reconnects)');"
        "if(!st){h.push('Status:    unavailable ('+err+')');}else{"
        "const f=st.firmware||{},m=st.memory||{};"
        "h.push('Firmware:  '+f.version+' (IDF '+f.idf_version+'), built '+f.compile_time);"
        "h.push('Uptime:    '+fmtDur(st.current_ms||0)+'   Free heap: '+m.free_heap+' (min '+m.min_free_heap+')'"
        "+'   WiFi RSSI: '+known(st.wifi_rssi,v=>v+' dBm'+(v<" STR(WIFI_RSSI_WEAK_DBM) "?' (weak)':'')));"
        "h.push('System:    heaters '+known(st.heaters_configured,String)+', solar '+known(st.has_solar,yn)"
        "+', separate pool/spa modes '+known(st.separate_pool_spa_modes,yn));"
        "const devs=st.devices||[];h.push('','Devices:');"
        "if(!devs.length)h.push('  (none seen yet)');"
        "const w=Math.max(4,...devs.map(d=>d.name.length));"
        "for(const d of devs){const c=d.message_counts||{};"
        "h.push('  '+d.address+'  '+d.name.padEnd(w)+'  fw '+String(d.firmware_version||'?').padEnd(6)"
        "+'decoded '+String(c.decoded).padStart(7)+'  unknown '+String(c.unknown).padStart(6));}"
        "h.push('','Message counts: '+(st.message_counts?messageSummary(st.message_counts):'unknown')"
        "+', resyncs '+resyncSummary(st.resyncs||{}));}"
        // Unknown-message signatures, most hits first: already deduplicated,
        // so a capture arrives with its own summary of protocol gaps. Broken
        // frames are left out: this is for valid frames the decoder doesn't
        // handle yet, and resyncs are already counted above.
        "h.push('');"
        "if(!unk.data){h.push('Unknown signatures: unavailable ('+unk.err+')');}else{"
        "const es=(unk.data.entries||[]).filter(e=>!e.is_error).sort((a,b)=>b.hits-a.hits);"
        "const tot=es.reduce((s,e)=>s+e.hits,0);"
        "h.push('Unknown signatures (valid frames, most hits first): '+es.length+' held, '+tot+' hits');"
        "if(es.length){"
        "const sw=Math.max(...es.map(e=>e.src_name.length)),dw=Math.max(...es.map(e=>e.dst_name.length));"
        "for(const e of es)h.push('  '+String(e.hits).padStart(7)+'  '+e.cmd+'  '+e.src_name.padEnd(sw)+' -> '+e.dst_name.padEnd(dw)"
        "+'  '+(e.payload||'(empty)')+(e.reason!=='unhandled'?'  ['+e.reason+']':'')"
        "+(e.raw_len>" STR(UNKNOWN_BUFFER_MAX_RAW_BYTES) "?'  (truncated, '+e.raw_len+' bytes)':''));}}"
        "return h.map(l=>l?'# '+l:'#').join('\\n')+'\\n# ---\\n';}"
        // Fetches JSON with a timeout, so a stalled request can't hold up the
        // download. Returns {data} or {err}.
        "async function getJson(u){try{const ac=new AbortController(),t=setTimeout(()=>ac.abort(),5000);"
        "const r=await fetch(u,{signal:ac.signal});clearTimeout(t);"
        "if(!r.ok)throw new Error('HTTP '+r.status);return{data:await r.json()};}"
        "catch(e){return{err:e.message||String(e)};}}"
        "document.getElementById('dl-btn').addEventListener('click',async function(){"
        "const d=new Date();const[s,unk]=await Promise.all([getJson('/status'),getJson('/unknown_msgs')]);"
        "const name=stampedName('pool-controller-log','txt',d);"
        "const text=header(s.data||null,s.err,unk,d)+lines.join('\\n')+(lines.length?'\\n':'')+partial;"
        "const url=URL.createObjectURL(new Blob([text],{type:'text/plain'}));"
        "const a=document.createElement('a');a.href=url;a.download=name;document.body.appendChild(a);a.click();"
        "a.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);});"
        "connect();status();"
        "</script>";

    return send_static_page(req, "Debug log", PAGE_LOG, page_content);
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

static const httpd_uri_t device_uri = {
    .uri = "/device",
    .method = HTTP_GET,
    .handler = device_get_handler
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

static const httpd_uri_t update_auto_check_uri = {
    .uri = "/update/auto_check",
    .method = HTTP_POST,
    .handler = update_auto_check_post_handler
};

static const httpd_uri_t usage_report_post_uri = {
    .uri = "/usage_report",
    .method = HTTP_POST,
    .handler = usage_report_post_handler
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

static const httpd_uri_t log_view_uri = {
    .uri = "/log_view",
    .method = HTTP_GET,
    .handler = log_view_handler
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
// main/static/oat.min.css  (https://oat.ink, MIT license)
extern const char _binary_oat_min_css_start[];
// main/static/app.css + app.js  (this project's theme and shared page helpers)
extern const char _binary_app_css_start[];
extern const char _binary_app_js_start[];

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
    // Pages link these with ?v=<build hash> (see send_page_start), so a
    // firmware update never leaves a browser on the previous build's copy.
    {
        .uri           = "/static/app.css",
        .content       = _binary_app_css_start,
        .content_end   = NULL,
        .content_type  = "text/css",
        .cache_control = "max-age=604800",
    },
    {
        .uri           = "/static/app.js",
        .content       = _binary_app_js_start,
        .content_end   = NULL,
        .content_type  = "application/javascript",
        .cache_control = "max-age=604800",
    },
};
#define STATIC_FILES_COUNT (sizeof(STATIC_FILES) / sizeof(STATIC_FILES[0]))

// --- Generic handler (shared by all static files via /static/* wildcard) ---

static esp_err_t static_file_handler(httpd_req_t *req)
{
    // Match the path only: req->uri includes any ?v= cache-busting query
    size_t path_len = strcspn(req->uri, "?");
    for (int i = 0; i < STATIC_FILES_COUNT; i++) {
        if (strlen(STATIC_FILES[i].uri) == path_len &&
            strncmp(req->uri, STATIC_FILES[i].uri, path_len) == 0) {
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
    httpd_register_uri_handler(server, &device_uri);
    httpd_register_uri_handler(server, &mqtt_config_get_uri);
    httpd_register_uri_handler(server, &mqtt_config_post_uri);
    httpd_register_uri_handler(server, &update_get_uri);
    httpd_register_uri_handler(server, &update_post_uri);
    httpd_register_uri_handler(server, &update_check_uri);
    httpd_register_uri_handler(server, &update_github_uri);
    httpd_register_uri_handler(server, &update_auto_check_uri);
    httpd_register_uri_handler(server, &usage_report_post_uri);
    httpd_register_uri_handler(server, &reboot_post_uri);
    httpd_register_uri_handler(server, &ha_reset_post_uri);
    httpd_register_uri_handler(server, &test_decode_uri);
    httpd_register_uri_handler(server, &unknown_msgs_json_uri);
    httpd_register_uri_handler(server, &unknown_msgs_clear_uri);
    httpd_register_uri_handler(server, &unknown_msgs_view_uri);
    httpd_register_uri_handler(server, &log_view_uri);
    web_log_register(server);                                 // /ws/log WebSocket
    httpd_register_uri_handler(server, &static_files_uri);  // /static/* wildcard
    httpd_register_uri_handler(server, &favicon_redirect_uri); // /favicon.ico -> /static/favicon.ico
    httpd_register_uri_handler(server, &robots_txt_uri);

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_done_event_handler, NULL);

    // Register custom 404 error handler for captive portal
    // This catches all unmatched URIs and redirects to the provisioning page
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    ESP_LOGI(TAG, "Web/HTTP handlers registered (with captive portal 404 redirect)");
    return ESP_OK;
}

