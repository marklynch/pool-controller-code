#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_err.h"

/**
 * @brief Start the captive portal DNS server
 *
 * This DNS server responds to all queries with the AP's IP address,
 * enabling captive portal functionality.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t dns_server_start(void);

/**
 * @brief Stop the captive portal DNS server
 */
void dns_server_stop(void);

#endif // DNS_SERVER_H
