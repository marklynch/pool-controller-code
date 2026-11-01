/**
 * Mock esp_http_client.h for host-based testing.
 *
 * Just the calls the firmware makes; the test supplies the functions, so it
 * can record each request and choose how it turns out.
 */

#ifndef ESP_HTTP_CLIENT_H
#define ESP_HTTP_CLIENT_H

#include "esp_err.h"

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
} esp_http_client_method_t;

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void *conf);
} esp_http_client_config_t;

typedef struct esp_http_client *esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

#endif // ESP_HTTP_CLIENT_H
