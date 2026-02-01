#include "tcp_bridge.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TCP_BRIDGE";

// Loopback tracking for TX echo detection
static uint8_t s_last_tx_msg[256];
static int s_last_tx_len = 0;
static TickType_t s_last_tx_time = 0;

// Bridge configuration (copied from user config)
static tcp_bridge_config_t s_config = {0};

// Task handle for cleanup
static TaskHandle_t s_bridge_task_handle = NULL;

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
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(listen_sock, (struct sockaddr *)&listen_addr,
                 sizeof(listen_addr)) < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (listen(listen_sock, 1) < 0) {
            ESP_LOGE(TAG, "Error during listen: errno %d", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
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
                    "Send hex strings (e.g., '02 00 50 FF FF 03') to transmit to the bus.\r\n\r\n";
                send(client_sock, hello, strlen(hello), 0);

                // Reset line buffer for new client
                line_pos = 0;
            }
        }

        // 1. UART RX - always read and log
        int len = s_config.uart_read(uart_buf, sizeof(uart_buf), 10);
        if (len > 0) {
            // Flash RX LED if callback provided
            if (s_config.led_flash_rx) {
                s_config.led_flash_rx();
            }

            // Format as hex string
            char hexLine[3 * TCP_UART_BUFFER_SIZE + 4];
            int pos = 0;
            for (int i = 0; i < len; ++i) {
                if (pos < (int)(sizeof(hexLine) - 4)) {
                    pos += snprintf(&hexLine[pos],
                                    sizeof(hexLine) - pos,
                                    "%02X ",
                                    uart_buf[i]);
                }
            }
            hexLine[pos] = '\0';

            // Check if this is our own transmitted message (loopback verification)
            bool is_loopback = false;
            if (s_last_tx_len > 0 && len == s_last_tx_len) {
                TickType_t time_since_tx = xTaskGetTickCount() - s_last_tx_time;
                if (time_since_tx < pdMS_TO_TICKS(500)) {  // Within 500ms of TX
                    if (memcmp(uart_buf, s_last_tx_msg, len) == 0) {
                        is_loopback = true;
                        ESP_LOGI(TAG, "RX LOOPBACK (our TX echoed): %s", hexLine);
                        s_last_tx_len = 0;  // Clear so we don't match again
                    }
                }
            }

            // Decode the message, log hex only if not decoded and not loopback
            if (!is_loopback && !s_config.decode_message(uart_buf, len)) {
                ESP_LOGI(TAG, "RX: %s", hexLine);
            }

            // Send to client if connected
            if (client_sock >= 0) {
                hexLine[pos++] = '\r';
                hexLine[pos++] = '\n';
                int sent = send(client_sock, hexLine, pos, 0);
                if (sent < 0) {
                    ESP_LOGW(TAG, "Client send error: errno %d", errno);
                    shutdown(client_sock, SHUT_RDWR);
                    close(client_sock);
                    client_sock = -1;
                }
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
                shutdown(client_sock, SHUT_RDWR);
                close(client_sock);
                client_sock = -1;
                line_pos = 0;  // Reset line buffer
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "Client recv error: errno %d", errno);
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
