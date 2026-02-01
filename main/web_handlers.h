#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include <esp_http_server.h>
#include "esp_err.h"

// TODO - these should be in provisioning function rather than here
// NVS provisioning keys
#define PROV_SOFTAP_SSID_PREFIX    "POOL_"
#define PROV_NVS_NAMESPACE         "wifi_config"
#define PROV_NVS_KEY_SSID          "ssid"
#define PROV_NVS_KEY_PASS          "password"

// WiFi credentials save function (defined in main.c, used by provisioning handler)
esp_err_t wifi_credentials_save(const char *ssid, const char *password);

// Register all HTTP handlers (unified for both provisioning and normal operation)
esp_err_t web_handlers_register(httpd_handle_t server);

#endif // WEB_HANDLERS_H
