#include "tcp_bridge.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *TAG = "TCP_BRIDGE";

// Loopback tracking for TX echo detection
static uint8_t s_last_tx_msg[256];
static int s_last_tx_len = 0;
static TickType_t s_last_tx_time = 0;

// Bridge configuration (copied from user config)
static tcp_bridge_config_t s_config = {0};

// Task handle for cleanup
static TaskHandle_t s_bridge_task_handle = NULL;

// Message reassembly buffer
static uint8_t s_msg_buffer[BUS_MESSAGE_MAX_SIZE];
static int s_msg_buffer_len = 0;

// Global client socket for log forwarding
static int s_log_client_sock = -1;
static SemaphoreHandle_t s_log_mutex = NULL;

// Original vprintf function
static vprintf_like_t s_original_vprintf = NULL;

/**
 * Custom vprintf that outputs to both console and TCP client
 */
static int tcp_bridge_vprintf(const char *fmt, va_list args)
{
    int len = 0;

    // Send to original output (console)
    if (s_original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        len = s_original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    // Format for TCP outside the mutex — vsnprintf is CPU-only, no reason to hold the lock for it
    if (s_log_mutex) {
        char log_buf[256];
        int tcp_len = vsnprintf(log_buf, sizeof(log_buf), fmt, args);

        // Lock only for the socket read + send
        if (tcp_len > 0 && xSemaphoreTake(s_log_mutex, 0) == pdTRUE) {
            if (s_log_client_sock >= 0) {
                send(s_log_client_sock, log_buf, tcp_len, MSG_DONTWAIT);
            }
            xSemaphoreGive(s_log_mutex);
        }
    }

    return len;
}

/**
 * Update the client socket for log forwarding
 */
static void tcp_bridge_set_log_client(int sock)
{
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_log_client_sock = sock;
        xSemaphoreGive(s_log_mutex);
    }
}

/**
 * Extract and process one complete message from the reassembly buffer.
 * Uses message structure validation and checksum to detect message boundaries.
 * Returns true if a message was found and processed.
 */
static bool extract_and_process_message(int client_sock)
{
    // Need at least minimum message: START + SRC + DST + CTRL + CMD(3) + CHK + END = 12 bytes
    if (s_msg_buffer_len < 12) {
        return false;
    }

    // Find start byte (0x02)
    int start_idx = -1;
    for (int i = 0; i < s_msg_buffer_len; i++) {
        if (s_msg_buffer[i] == 0x02) {
            start_idx = i;
            break;
        }
    }

    // No start byte found - discard everything
    if (start_idx == -1) {
        if (s_msg_buffer_len > 0) {
            char hex_str[100];
            int hex_pos = 0;
            int dump_len = (s_msg_buffer_len < 32) ? s_msg_buffer_len : 32;
            for (int i = 0; i < dump_len && hex_pos < (int)sizeof(hex_str) - 3; i++) {
                hex_pos += snprintf(&hex_str[hex_pos], sizeof(hex_str) - hex_pos, "%02X ", s_msg_buffer[i]);
            }
            hex_str[hex_pos] = '\0';
            ESP_LOGW(TAG, "No start byte in buffer, discarding %d bytes: %s%s",
                     s_msg_buffer_len, hex_str, s_msg_buffer_len > 32 ? "..." : "");
        }
        s_msg_buffer_len = 0;
        return false;
    }

    // Discard bytes before start
    if (start_idx > 0) {
        memmove(s_msg_buffer, &s_msg_buffer[start_idx], s_msg_buffer_len - start_idx);
        s_msg_buffer_len -= start_idx;
    }

    // Check we have enough for header validation
    if (s_msg_buffer_len < 10) {
        return false;  // Wait for more data
    }

    // Validate control bytes (positions 5-6 should be 0x80 0x00)
    if (s_msg_buffer[5] != 0x80 || s_msg_buffer[6] != 0x00) {
        char hex_str[100];
        int hex_pos = 0;
        int dump_len = (s_msg_buffer_len < 32) ? s_msg_buffer_len : 32;
        for (int i = 0; i < dump_len && hex_pos < (int)sizeof(hex_str) - 3; i++) {
            hex_pos += snprintf(&hex_str[hex_pos], sizeof(hex_str) - hex_pos, "%02X ", s_msg_buffer[i]);
        }
        hex_str[hex_pos] = '\0';
        ESP_LOGW(TAG, "Invalid control bytes: %02X %02X (expected 80 00), data: %s%s, discarding start byte",
                 s_msg_buffer[5], s_msg_buffer[6], hex_str, s_msg_buffer_len > 32 ? "..." : "");
        // Discard this start byte and look for next
        memmove(s_msg_buffer, &s_msg_buffer[1], s_msg_buffer_len - 1);
        s_msg_buffer_len--;
        return false;
    }

    // Now scan for message end using checksum validation
    // Data starts at index 10, checksum algorithm: sum(bytes 10..N-3) & 0xFF
    for (int pos = 10; pos < s_msg_buffer_len - 2; pos++) {  // Need at least 2 more bytes (checksum + END)
        // Calculate checksum from byte 10 to current position (inclusive)
        uint32_t sum = 0;
        for (int i = 10; i <= pos; i++) {
            sum += s_msg_buffer[i];
        }
        uint8_t calculated_checksum = sum & 0xFF;

        // Check if next byte matches checksum AND byte after that is END marker (0x03)
        // This prevents false positives from accidental checksum matches in payload
        if (s_msg_buffer[pos + 1] == calculated_checksum && s_msg_buffer[pos + 2] == 0x03) {
            // Found complete message!
            int msg_len = pos + 3;  // Include checksum and end byte

            // Format as hex string
            char hexLine[3 * BUS_MESSAGE_MAX_SIZE + 4];
            int hex_pos = 0;
            for (int i = 0; i < msg_len; i++) {
                if (hex_pos < (int)(sizeof(hexLine) - 4)) {
                    hex_pos += snprintf(&hexLine[hex_pos], sizeof(hexLine) - hex_pos,
                                      "%02X ", s_msg_buffer[i]);
                }
            }
            hexLine[hex_pos] = '\0';

            // Check for loopback
            bool is_loopback = false;
            if (s_last_tx_len > 0 && msg_len == s_last_tx_len) {
                TickType_t time_since_tx = xTaskGetTickCount() - s_last_tx_time;
                if (time_since_tx < pdMS_TO_TICKS(LOOPBACK_DETECTION_MS)) {
                    if (memcmp(s_msg_buffer, s_last_tx_msg, msg_len) == 0) {
                        is_loopback = true;
                        ESP_LOGI(TAG, "RX LOOPBACK (our TX echoed): %s", hexLine);
                        s_last_tx_len = 0;
                    }
                }
            }

            // Decode message, log hex only if not decoded and not loopback
            if (!is_loopback && !s_config.decode_message(s_msg_buffer, msg_len)) {
                ESP_LOGI(TAG, "RX: %s", hexLine);
            }

            // Send to TCP client if connected
            if (client_sock >= 0) {
                hexLine[hex_pos++] = '\r';
                hexLine[hex_pos++] = '\n';
                int sent = send(client_sock, hexLine, hex_pos, 0);
                if (sent < 0) {
                    ESP_LOGD(TAG, "Client send error: errno %d", errno);
                }
            }

            // Remove processed message from buffer
            int remaining = s_msg_buffer_len - msg_len;
            if (remaining > 0) {
                memmove(s_msg_buffer, &s_msg_buffer[msg_len], remaining);
            }
            s_msg_buffer_len = remaining;

            return true;  // Message processed
        }
    }

    // No complete message yet - check for buffer overflow
    if (s_msg_buffer_len >= BUS_MESSAGE_MAX_SIZE - 10) {
        char hex_str[100];
        int hex_pos = 0;
        int dump_len = 32;  // Show first 32 bytes
        for (int i = 0; i < dump_len && hex_pos < (int)sizeof(hex_str) - 3; i++) {
            hex_pos += snprintf(&hex_str[hex_pos], sizeof(hex_str) - hex_pos, "%02X ", s_msg_buffer[i]);
        }
        hex_str[hex_pos] = '\0';
        ESP_LOGW(TAG, "Buffer nearly full (%d bytes) without complete message, first 32 bytes: %s..., clearing",
                 s_msg_buffer_len, hex_str);
        s_msg_buffer_len = 0;
    }

    return false;  // Wait for more data
}

/**
 * TCP server task implementation
 */
static void tcp_bridge_task(void *pvParameters)
{
    (void)pvParameters;

    char addr_str[128];
    int listen_sock = -1;
    int client_sock = -1;
    uint8_t uart_buf[TCP_UART_BUFFER_SIZE];
    uint8_t tcp_buf[TCP_BUFFER_SIZE];
    char line_buf[TCP_LINE_BUFFER_SIZE];
    int line_pos = 0;

    // Create listening socket
    while (listen_sock < 0) {
        struct sockaddr_in listen_addr = {0};
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_addr.sin_port = htons(s_config.port);

        listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(listen_sock, (struct sockaddr *)&listen_addr,
                 sizeof(listen_addr)) < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
            continue;
        }

        if (listen(listen_sock, 1) < 0) {
            ESP_LOGE(TAG, "Error during listen: errno %d", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
            continue;
        }

        // Make listening socket non-blocking
        int flags = fcntl(listen_sock, F_GETFL, 0);
        fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK);

        ESP_LOGI(TAG, "TCP bridge listening on port %d", s_config.port);
    }

    // Main loop - always reads UART, optionally bridges to TCP client
    while (1) {
        // Check for new client connection (non-blocking)
        if (client_sock < 0) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            client_sock = accept(listen_sock,
                                 (struct sockaddr *)&client_addr,
                                 &addr_len);
            if (client_sock >= 0) {
                inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
                ESP_LOGI(TAG, "Client connected from %s", addr_str);

                const char *hello =
                    "Connected to ESP32-C6 pool bus bridge.\r\n"
                    "UART bytes will be shown here in hex.\r\n"
                    "Decoded messages will also be shown.\r\n"
                    "Send hex strings (e.g., '02 00 50 FF FF 03') to transmit to the bus.\r\n\r\n";
                send(client_sock, hello, strlen(hello), 0);

                // Enable log forwarding for this client
                tcp_bridge_set_log_client(client_sock);

                // Reset line buffer for new client
                line_pos = 0;
            }
        }

        // 1. UART RX - accumulate into reassembly buffer
        int len = s_config.uart_read(uart_buf, sizeof(uart_buf), UART_RX_TIMEOUT_MS);
        if (len > 0) {
            // Flash RX LED if callback provided
            if (s_config.led_flash_rx) {
                s_config.led_flash_rx();
            }

            // Append to reassembly buffer
            if (s_msg_buffer_len + len <= BUS_MESSAGE_MAX_SIZE) {
                memcpy(&s_msg_buffer[s_msg_buffer_len], uart_buf, len);
                s_msg_buffer_len += len;
            } else {
                ESP_LOGW(TAG, "Reassembly buffer overflow (%d + %d > %d), clearing",
                         s_msg_buffer_len, len, BUS_MESSAGE_MAX_SIZE);
                s_msg_buffer_len = 0;
            }

            // Extract and process all complete messages
            while (extract_and_process_message(client_sock)) {
                // Keep extracting until no more complete messages
            }
        }

        // 2. TCP -> UART (only if client connected)
        if (client_sock >= 0) {
            int r = recv(client_sock,
                         tcp_buf,
                         sizeof(tcp_buf),
                         MSG_DONTWAIT);
            if (r > 0) {
                // Process received characters
                for (int i = 0; i < r; i++) {
                    char c = tcp_buf[i];

                    // Echo the character back to the client
                    send(client_sock, &c, 1, 0);

                    if (c == '\n' || c == '\r') {
                        // End of line - process the accumulated command
                        if (line_pos > 0) {
                            line_buf[line_pos] = '\0';

                            // Parse hex string for loopback tracking (before sending)
                            s_last_tx_len = 0;
                            const char *p = line_buf;
                            while (*p != '\0' && s_last_tx_len < (int)sizeof(s_last_tx_msg)) {
                                // Skip whitespace
                                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                                if (*p == '\0') break;

                                // Parse two hex digits
                                if (p[0] && p[1]) {
                                    char hex_byte[3] = {p[0], p[1], 0};
                                    unsigned long val = strtoul(hex_byte, NULL, 16);
                                    s_last_tx_msg[s_last_tx_len++] = (uint8_t)val;
                                    p += 2;
                                } else {
                                    break;
                                }
                            }

                            // Store timestamp for loopback verification
                            s_last_tx_time = xTaskGetTickCount();

                            // Parse and send the hex string
                            int sent = s_config.uart_write(line_buf);
                            if (sent > 0) {
                                const char *ok_msg = "OK - sent\r\n";
                                send(client_sock, ok_msg, strlen(ok_msg), 0);

                                // Decode the sent message (will be logged via custom vprintf)
                                if (s_config.decode_message && s_last_tx_len > 0) {
                                    s_config.decode_message(s_last_tx_msg, s_last_tx_len);
                                }

                                // Flash TX LED if callback provided
                                if (s_config.led_flash_tx) {
                                    s_config.led_flash_tx();
                                }
                            } else {
                                const char *err_msg = "ERROR - invalid hex string\r\n";
                                send(client_sock, err_msg, strlen(err_msg), 0);
                                s_last_tx_len = 0;  // Clear on error
                            }

                            line_pos = 0;  // Reset for next line
                        }
                    } else if (c == 0x08 || c == 0x7F) {
                        // Backspace or delete - remove last character
                        if (line_pos > 0) {
                            line_pos--;
                        }
                    } else {
                        // Add character to line buffer
                        if (line_pos < (int)sizeof(line_buf) - 1) {
                            line_buf[line_pos++] = c;
                        } else {
                            // Buffer full - reset
                            const char *overflow_msg = "\r\nERROR - line too long\r\n";
                            send(client_sock, overflow_msg, strlen(overflow_msg), 0);
                            line_pos = 0;
                        }
                    }
                }
            } else if (r == 0) {
                ESP_LOGI(TAG, "Client disconnected");
                tcp_bridge_set_log_client(-1);  // Disable log forwarding
                shutdown(client_sock, SHUT_RDWR);
                close(client_sock);
                client_sock = -1;
                line_pos = 0;  // Reset line buffer
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "Client recv error: errno %d", errno);
                    tcp_bridge_set_log_client(-1);  // Disable log forwarding
                    shutdown(client_sock, SHUT_RDWR);
                    close(client_sock);
                    client_sock = -1;
                    line_pos = 0;  // Reset line buffer
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Cleanup (never reached in current implementation)
    if (client_sock >= 0) {
        close(client_sock);
    }
    if (listen_sock >= 0) {
        close(listen_sock);
    }
    vTaskDelete(NULL);
}

esp_err_t tcp_bridge_start(const tcp_bridge_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Config cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!config->uart_read || !config->uart_write || !config->decode_message) {
        ESP_LOGE(TAG, "Required callbacks not provided");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_bridge_task_handle != NULL) {
        ESP_LOGW(TAG, "TCP bridge already running");
        return ESP_ERR_INVALID_STATE;
    }

    // Copy configuration
    memcpy(&s_config, config, sizeof(tcp_bridge_config_t));

    // Initialize log forwarding
    if (!s_log_mutex) {
        s_log_mutex = xSemaphoreCreateMutex();
        if (!s_log_mutex) {
            ESP_LOGE(TAG, "Failed to create log mutex");
            return ESP_ERR_NO_MEM;
        }

        // Install custom vprintf to forward logs to TCP client
        s_original_vprintf = esp_log_set_vprintf(tcp_bridge_vprintf);
        ESP_LOGI(TAG, "Log forwarding to TCP enabled");
    }

    // Create bridge task
    BaseType_t result = xTaskCreate(
        tcp_bridge_task,
        "tcp_bridge",
        TCP_TASK_STACK_SIZE,
        NULL,
        TCP_TASK_PRIORITY,
        &s_bridge_task_handle
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP bridge task");
        s_bridge_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TCP bridge started successfully");
    return ESP_OK;
}

esp_err_t tcp_bridge_stop(void)
{
    if (s_bridge_task_handle == NULL) {
        ESP_LOGW(TAG, "TCP bridge not running");
        return ESP_ERR_INVALID_STATE;
    }

    // Delete task
    vTaskDelete(s_bridge_task_handle);
    s_bridge_task_handle = NULL;

    ESP_LOGI(TAG, "TCP bridge stopped");
    return ESP_OK;
}
