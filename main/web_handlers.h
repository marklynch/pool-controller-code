#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include <esp_http_server.h>
#include "esp_err.h"

// Register all HTTP handlers (unified for both provisioning and normal operation)
esp_err_t web_handlers_register(httpd_handle_t server);

#endif // WEB_HANDLERS_H
