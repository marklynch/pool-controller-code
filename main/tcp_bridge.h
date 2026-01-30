#ifndef TCP_BRIDGE_H
#define TCP_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Callback function types for TCP bridge
 */

// Called when UART data needs to be read
// Returns number of bytes read, or 0 if none available
typedef int (*tcp_bridge_uart_read_fn)(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);

// Called when a hex string command should be sent to UART
// Returns number of bytes sent, or -1 on error
typedef int (*tcp_bridge_uart_write_fn)(const char *hex_string);

// Called when a message is received from UART for decoding/logging
// Returns true if message was decoded (suppress raw hex output)
typedef bool (*tcp_bridge_decode_fn)(const uint8_t *data, int len);

// Called when RX or TX LED should flash
typedef void (*tcp_bridge_led_flash_fn)(void);

/**
 * Configuration structure for TCP bridge
 */
typedef struct {
    uint16_t port;                           // TCP port to listen on
    tcp_bridge_uart_read_fn uart_read;       // UART read callback
    tcp_bridge_uart_write_fn uart_write;     // UART write callback
    tcp_bridge_decode_fn decode_message;     // Message decoder callback
    tcp_bridge_led_flash_fn led_flash_rx;    // RX LED callback (can be NULL)
    tcp_bridge_led_flash_fn led_flash_tx;    // TX LED callback (can be NULL)
} tcp_bridge_config_t;

/**
 * Start the TCP bridge server
 *
 * Creates a FreeRTOS task that:
 * - Listens for TCP clients on the configured port
 * - Forwards UART data to connected TCP clients as hex strings
 * - Accepts hex string commands from TCP clients and sends to UART
 *
 * @param config Bridge configuration (copied internally, can be freed after call)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tcp_bridge_start(const tcp_bridge_config_t *config);

/**
 * Stop the TCP bridge server
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tcp_bridge_stop(void);

#endif // TCP_BRIDGE_H
