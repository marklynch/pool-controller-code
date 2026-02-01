#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include "esp_err.h"
#include <stdbool.h>
#include <esp_http_server.h>

// NVS provisioning keys
#define PROV_SOFTAP_SSID_PREFIX    "POOL_"
#define PROV_NVS_NAMESPACE         "wifi_config"
#define PROV_NVS_KEY_SSID          "ssid"
#define PROV_NVS_KEY_PASS          "password"

/**
 * Initialize WiFi and provisioning system
 * - If credentials exist: connects to WiFi in station mode
 * - If no credentials: starts SoftAP for provisioning
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_init(void);

/**
 * Save WiFi credentials to NVS
 *
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return ESP_OK on success
 */
esp_err_t wifi_credentials_save(const char *ssid, const char *password);

/**
 * Check if device is in provisioning mode
 *
 * @return true if in provisioning mode (SoftAP active)
 */
bool wifi_is_provisioning_active(void);

/**
 * Check if WiFi is connected
 *
 * @return true if connected to WiFi
 */
bool wifi_is_connected(void);

/**
 * Get current device IP address
 *
 * @return IP address string (e.g., "192.168.0.123") or empty string if not connected
 */
const char* wifi_get_device_ip(void);

/**
 * Wait for WiFi connection
 * Blocks until WiFi is connected or provisioning is active
 */
void wifi_wait_for_connection(void);

/**
 * Register HTTP server handle for web interface
 * Should be called after wifi_provisioning_init()
 *
 * @param server HTTP server handle
 * @return ESP_OK on success
 */
esp_err_t wifi_register_http_server(httpd_handle_t server);

#endif // WIFI_PROVISIONING_H
