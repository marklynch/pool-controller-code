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

// Resync events emitted by the reassembly layer. Each value maps 1:1 to a
// framing_result_t failure so the breakdown survives to the status page.
// NO_START and BUFFER_OVERFLOW discard a run of bytes; the BAD_* variants step
// the window forward by a single byte and retry.
typedef enum {
    TCP_BRIDGE_RESYNC_NO_START,            // No START byte (0x02) in buffer, all bytes discarded
    TCP_BRIDGE_RESYNC_BAD_HEADER_CHECKSUM, // Header checksum (byte 9) mismatch
    TCP_BRIDGE_RESYNC_BAD_FRAME_TYPE,      // Frame type bytes (5-6) not 80 00 or 00 00
    TCP_BRIDGE_RESYNC_BAD_LENGTH,          // Length field (byte 8) out of range
    TCP_BRIDGE_RESYNC_BAD_END,             // End byte != 0x03 at declared length
    TCP_BRIDGE_RESYNC_BAD_DATA_CHECKSUM,   // Data checksum mismatch
    TCP_BRIDGE_RESYNC_BUFFER_OVERFLOW,     // Buffer filled without a complete frame (too long / corrupt)
} tcp_bridge_resync_type_t;

// Called when the reassembly layer resyncs (discards data) after a framing failure.
// data/len are the bytes that triggered the resync, passed for every result type
// (NO_START, BUFFER_OVERFLOW, and all BAD_* steps), so the caller can capture the
// lost frame for diagnostics.
typedef void (*tcp_bridge_resync_fn)(tcp_bridge_resync_type_t type, const uint8_t *data, int len);

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
    tcp_bridge_resync_fn on_resync;          // Framing resync callback (can be NULL)
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
