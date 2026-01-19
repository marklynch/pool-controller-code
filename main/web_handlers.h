#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include <esp_http_server.h>
#include "esp_err.h"

// WiFi credentials save function (defined in main.c, used by provisioning handler)
esp_err_t wifi_credentials_save(const char *ssid, const char *password);


// Register all HTTP handlers for currently same for provisioning mode (AP mode) and normal mode (station mode)
esp_err_t web_handlers_register(httpd_handle_t server);

#endif // WEB_HANDLERS_H
