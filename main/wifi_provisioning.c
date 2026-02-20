#include "wifi_provisioning.h"
#include "config.h"
#include "web_handlers.h"
#include "mqtt_poolclient.h"
#include "led_helper.h"
#include "dns_server.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
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

// ======================================================
// Forward Declarations
// ======================================================

static esp_err_t start_http_server(const char *ip_address);
static esp_err_t start_provisioning(void);

// ======================================================
// WiFi Credential Management
// ======================================================

static bool wifi_credentials_exist(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, WIFI_PROV_NVS_KEY_SSID, NULL, &required_size);
    nvs_close(nvs_handle);

    return (err == ESP_OK && required_size > 0);
}

static esp_err_t wifi_credentials_load(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;

    err = nvs_get_str(nvs_handle, WIFI_PROV_NVS_KEY_SSID, ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs_handle, WIFI_PROV_NVS_KEY_PASS, password, &pass_size);
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t wifi_credentials_clear(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_key(nvs_handle, WIFI_PROV_NVS_KEY_SSID);
    nvs_erase_key(nvs_handle, WIFI_PROV_NVS_KEY_PASS);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
    return ESP_OK;
}

esp_err_t wifi_credentials_save(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_PROV_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, WIFI_PROV_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, WIFI_PROV_NVS_KEY_PASS, password);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
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
    ESP_ERROR_CHECK(mdns_init());

    // Set mDNS hostname (will be accessible as <hostname>.local)
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));

    // Set default instance name
    ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE_NAME));

    ESP_LOGI(TAG, "mDNS started - accessible at %s.local", MDNS_HOSTNAME);

    // Advertise HTTP service
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", HTTP_SERVER_PORT, NULL, 0));
    ESP_LOGI(TAG, "  - HTTP service: http://%s.local:%d", MDNS_HOSTNAME, HTTP_SERVER_PORT);

    // Advertise TCP bridge service (custom service type)
    mdns_txt_item_t tcp_bridge_txt[] = {
        {"protocol", "astral-pool-bus"},
        {"version", "1.0"}
    };
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_pool-bridge", "_tcp", TCP_BRIDGE_PORT,
                                     tcp_bridge_txt, sizeof(tcp_bridge_txt) / sizeof(mdns_txt_item_t)));
    ESP_LOGI(TAG, "  - Pool Bridge service: tcp://%s.local:%d", MDNS_HOSTNAME, TCP_BRIDGE_PORT);
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
        esp_wifi_connect();
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
                    wifi_credentials_clear();
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
    // Load credentials from NVS if available and configure STA
    if (wifi_credentials_exist()) {
        char ssid[33] = {0};
        char password[64] = {0};
        esp_err_t err = wifi_credentials_load(ssid, sizeof(ssid), password, sizeof(password));
        if (err == ESP_OK) {
            wifi_config_t wifi_cfg = {0};
            memcpy(wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
            memcpy(wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
            ESP_LOGI(TAG, "WiFi credentials found, connecting to SSID: %s", ssid);
        } else {
            ESP_LOGW(TAG, "Failed to load credentials from NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "No WiFi credentials in NVS, trying with driver's saved config...");
    }

    // Always start in STA mode - SoftAP provisioning only starts if connection fails
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
