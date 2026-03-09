#include "wifi_provisioning.h"
#include "config.h"
#include "web_handlers.h"
#include "mqtt_poolclient.h"
#include "led_helper.h"
#include "dns_server.h"
#include "device_serial.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include <esp_http_server.h>
#include "mdns.h"

static const char *TAG = "WIFI_PROV";

// WiFi event bits
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// State variables
static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_provisioning_active = false;
static bool s_wifi_connected = false;
static bool s_wifi_ever_connected = false;
static httpd_handle_t s_httpd_handle = NULL;
static int s_wifi_retry_count = 0;
static TimerHandle_t s_wifi_retry_timer = NULL;
static char s_device_ip_address[16] = {0};
static char s_mdns_hostname[64] = {0};

// ======================================================
// Forward Declarations
// ======================================================

static esp_err_t start_http_server(const char *ip_address);
static esp_err_t start_provisioning(void);

// ======================================================
// WiFi Credential Management
// ======================================================

esp_err_t wifi_credentials_save(const char *ssid, const char *password)
{
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
}

// ======================================================
// WiFi Retry Timer
// ======================================================

static void wifi_retry_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Retry timer expired, attempting reconnection (attempt %d/%d)...",
             s_wifi_retry_count, WIFI_MAX_RETRY_ATTEMPTS);
    esp_wifi_connect();
}

// ======================================================
// mDNS Service
// ======================================================

static void start_mdns_service(void)
{
    // Initialize mDNS
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed (%s) — skipping mDNS", esp_err_to_name(err));
        return;
    }

    // Build unique hostname and instance names from last 3 MAC bytes
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    char hostname[32];
    char instance_name[32];
    char debug_instance_name[40];
    snprintf(hostname,            sizeof(hostname),
             "%s-%02X%02X%02X", MDNS_HOSTNAME, mac[3], mac[4], mac[5]);
    snprintf(instance_name,       sizeof(instance_name),
             "%s %02X%02X%02X", MDNS_INSTANCE_NAME, mac[3], mac[4], mac[5]);
    snprintf(debug_instance_name, sizeof(debug_instance_name),
             "%s %02X%02X%02X", MDNS_INSTANCE_DEBUG_NAME, mac[3], mac[4], mac[5]);

    // Cache hostname for use elsewhere (e.g. web UI)
    strncpy(s_mdns_hostname, hostname, sizeof(s_mdns_hostname) - 1);
    s_mdns_hostname[sizeof(s_mdns_hostname) - 1] = '\0';

    // ESP-IDF requires hostname to be set before instance name
    if (mdns_hostname_set(hostname) != ESP_OK ||
        mdns_instance_name_set(instance_name) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname/instance setup failed — mDNS may be unavailable");
    }

    ESP_LOGI(TAG, "mDNS started - accessible at %s.local (%s)", hostname, instance_name);

    // Shared TXT record values
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char serial[DEVICE_SERIAL_LEN];
    device_get_serial(serial, sizeof(serial));

    // Advertise HTTP service
    mdns_txt_item_t http_txt[] = {
        {"id", serial},
        {"fw", app_desc->version},
    };
    if (mdns_service_add(instance_name, "_http", "_tcp", HTTP_SERVER_PORT,
                         http_txt, sizeof(http_txt) / sizeof(mdns_txt_item_t)) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: failed to register HTTP service");
    }
    ESP_LOGI(TAG, "  - HTTP service: http://%s.local:%d", hostname, HTTP_SERVER_PORT);

    // Advertise TCP bridge service (custom service type)
    mdns_txt_item_t tcp_bridge_txt[] = {
        {"protocol", "pool-controller-bus"},
        {"version",  "1.0"},
        {"id", serial},
        {"fw", app_desc->version},
    };
    if (mdns_service_add(debug_instance_name, "_pool-bridge", "_tcp", TCP_BRIDGE_PORT,
                         tcp_bridge_txt, sizeof(tcp_bridge_txt) / sizeof(mdns_txt_item_t)) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: failed to register pool-bridge service");
    }
    ESP_LOGI(TAG, "  - Pool Bridge service: tcp://%s.local:%d (%s)", hostname, TCP_BRIDGE_PORT, debug_instance_name);
}

const char *wifi_get_mdns_hostname(void)
{
    return s_mdns_hostname;
}

// ======================================================
// WiFi Event Handler
// ======================================================

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_config_t cfg = {0};
        esp_wifi_get_config(WIFI_IF_STA, &cfg);
        if (cfg.sta.ssid[0] != '\0') {
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "No WiFi credentials - starting provisioning");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_device_ip_address[0] = '\0';
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Stop MQTT client on WiFi disconnect
        mqtt_client_stop();

        if (!s_provisioning_active) {
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected (attempt %d/%d)",
                     s_wifi_retry_count, WIFI_MAX_RETRY_ATTEMPTS);

            if (s_wifi_retry_count >= WIFI_MAX_RETRY_ATTEMPTS) {
                if (!s_wifi_ever_connected) {
                    // Initial connection failed - signal to start provisioning mode
                    ESP_LOGW(TAG, "Initial WiFi connection failed after %d attempts - starting provisioning",
                             WIFI_MAX_RETRY_ATTEMPTS);
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                } else {
                    // Re-connection failed after being previously connected - clear and restart
                    ESP_LOGE(TAG, "WiFi re-connection failed after %d attempts - clearing credentials and restarting",
                             WIFI_MAX_RETRY_ATTEMPTS);
                    wifi_config_t empty_cfg = {0};
                    esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
                    vTaskDelay(pdMS_TO_TICKS(WIFI_RESTART_DELAY_MS));
                    esp_restart();
                }
            } else {
                ESP_LOGI(TAG, "Will retry connection in %d seconds...", WIFI_RETRY_DELAY_MS / 1000);
                if (s_wifi_retry_timer != NULL) {
                    xTimerStart(s_wifi_retry_timer, 0);
                }
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        // Store IP address
        snprintf(s_device_ip_address, sizeof(s_device_ip_address),
                 IPSTR, IP2STR(&event->ip_info.ip));

        ESP_LOGI(TAG, "Got IP: %s", s_device_ip_address);
        s_wifi_connected = true;
        s_wifi_ever_connected = true;
        s_wifi_retry_count = 0;

        // If we were in provisioning mode, stop it now
        if (s_provisioning_active) {
            ESP_LOGI(TAG, "Exiting provisioning mode");
            s_provisioning_active = false;

            // Stop DNS server
            dns_server_stop();

            // Switch from APSTA to STA mode
            esp_wifi_set_mode(WIFI_MODE_STA);
        }

        // Stop retry timer if running
        if (s_wifi_retry_timer != NULL && xTimerIsTimerActive(s_wifi_retry_timer)) {
            xTimerStop(s_wifi_retry_timer, 0);
        }

        led_set_connected();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Start MQTT client on WiFi connect
        mqtt_client_start();

        // Start mDNS service for network discovery
        start_mdns_service();
    }
}

// ======================================================
// HTTP Server
// ======================================================

static esp_err_t start_http_server(const char *ip_address)
{
    if (s_httpd_handle != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = HTTP_SERVER_PORT;
    httpd_config.max_uri_handlers = HTTP_MAX_URI_HANDLERS;
    httpd_config.recv_wait_timeout = HTTP_RECV_TIMEOUT_SEC;
    httpd_config.send_wait_timeout = HTTP_SEND_TIMEOUT_SEC;
    httpd_config.stack_size = HTTP_STACK_SIZE;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd_handle, &httpd_config);
    if (err == ESP_OK) {
        web_handlers_register(s_httpd_handle);
        ESP_LOGI(TAG, "HTTP server started at http://%s", ip_address);
        ESP_LOGI(TAG, "  - WiFi config: http://%s/", ip_address);
        ESP_LOGI(TAG, "  - Pool status: http://%s/status", ip_address);
        ESP_LOGI(TAG, "  - MQTT config: http://%s/mqtt_config", ip_address);
        ESP_LOGI(TAG, "  - Firmware update: http://%s/update", ip_address);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
    }

    return err;
}

// ======================================================
// WiFi Initialization
// ======================================================

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             WIFI_PROV_SOFTAP_SSID_PREFIX, eth_mac[3], eth_mac[4], eth_mac[5]);
}

static esp_err_t start_softap_provisioning(void)
{
    char ap_ssid[32];
    get_device_service_name(ap_ssid, sizeof(ap_ssid));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    memcpy(wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    memcpy(wifi_config.ap.password, WIFI_PROV_SOFTAP_PASSWORD, strlen(WIFI_PROV_SOFTAP_PASSWORD));

    // Stop WiFi, switch to APSTA mode, restart
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started - SSID: %s, Password: %s", ap_ssid, WIFI_PROV_SOFTAP_PASSWORD);

    esp_err_t dns_err = dns_server_start();
    if (dns_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start DNS server: %s", esp_err_to_name(dns_err));
    } else {
        ESP_LOGI(TAG, "Captive portal DNS server started");
    }

    return start_http_server(WIFI_PROV_SOFTAP_IP);
}

static esp_err_t start_provisioning(void)
{
    wifi_config_t wifi_cfg = {0};
    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
    if (wifi_cfg.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "WiFi credentials found, connecting to SSID: %s", wifi_cfg.sta.ssid);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials configured");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

// ======================================================
// Public API
// ======================================================

esp_err_t wifi_provisioning_init(void)
{
    // Create event group
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    // Create WiFi retry timer (one-shot timer)
    if (s_wifi_retry_timer == NULL) {
        s_wifi_retry_timer = xTimerCreate("wifi_retry",
                                          pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS),
                                          pdFALSE,
                                          NULL,
                                          wifi_retry_timer_callback);
        if (s_wifi_retry_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create WiFi retry timer");
        }
    }

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_LOGI(TAG, "WiFi initialization complete");

    // Start provisioning
    return start_provisioning();
}

bool wifi_is_provisioning_active(void)
{
    return s_provisioning_active;
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

const char* wifi_get_device_ip(void)
{
    return s_device_ip_address;
}

void wifi_wait_for_connection(void)
{
    ESP_LOGI(TAG, "Waiting for WiFi connection...");

    // Wait for either a successful connection or all retries exhausted
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        start_http_server(s_device_ip_address);
        return;
    }

    // Connection failed after all retries - start SoftAP provisioning mode
    ESP_LOGW(TAG, "WiFi unavailable, starting provisioning mode");
    s_provisioning_active = true;
    s_wifi_retry_count = 0;
    led_set_unconfigured();

    start_softap_provisioning();

    char ap_ssid[32];
    get_device_service_name(ap_ssid, sizeof(ap_ssid));
    ESP_LOGI(TAG, "Provisioning mode active - connect to '%s' (password: '%s') and navigate to http://%s",
             ap_ssid, WIFI_PROV_SOFTAP_PASSWORD, WIFI_PROV_SOFTAP_IP);

    // Wait indefinitely until the user provisions credentials (device will restart after save)
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
}
