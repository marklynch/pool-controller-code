#include "tcp_bridge.h"
#include "framing.h"
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

// The buffer-overflow recovery path below re-adds the just-read chunk into a
// freshly reset framing buffer and ignores the return value, which is only
// safe because a single UART read can never itself exceed the framing
// buffer's capacity. Guard the assumption so a future config change can't
// silently break it.
_Static_assert(TCP_UART_BUFFER_SIZE <= BUS_MESSAGE_MAX_SIZE,
               "TCP_UART_BUFFER_SIZE must not exceed BUS_MESSAGE_MAX_SIZE, or the "
               "post-overflow re-add of a UART chunk in the bridge task can fail");

// Loopback tracking for TX echo detection
static uint8_t s_last_tx_msg[BUS_MESSAGE_MAX_SIZE];
static int s_last_tx_len = 0;
static TickType_t s_last_tx_time = 0;

// Bridge configuration (copied from user config)
static tcp_bridge_config_t s_config = {0};

// Task handle for cleanup
static TaskHandle_t s_bridge_task_handle = NULL;

// Message reassembly buffer
static framing_buffer_t s_framing_buffer;

// Global client socket for log forwarding
static int s_log_client_sock = -1;
static SemaphoreHandle_t s_log_mutex = NULL;

// Original vprintf function
static vprintf_like_t s_original_vprintf = NULL;

// Stop coordination
static volatile bool s_stop_requested = false;
static SemaphoreHandle_t s_stopped_sem = NULL;

/**
 * Send data to the TCP client under the client mutex.
 * All sends to client_sock must go through this to prevent interleaving
 * with the log vprintf callback, which can fire from any task at any time.
 *
 * The client socket is non-blocking, so a stalled or dead peer can never wedge
 * the bridge task. Returns true if the client should be kept, false if it
 * should be dropped:
 *   - full send            -> keep
 *   - buffer full (EAGAIN) -> message dropped, keep. The hex stream is a
 *                             best-effort debug feed; a truly dead peer is
 *                             reaped by TCP keepalive / the recv path.
 *   - partial write        -> drop, since the hex stream is now desynced and a
 *                             clean reconnect is the safe recovery.
 *   - other send error     -> drop.
 * Mutex contention is never the client's fault, so it keeps the client.
 */
static bool send_to_client(int sock, const void *data, int len)
{
    if (sock < 0 || len <= 0) {
        return true;
    }
    bool keep = true;
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        int sent = send(sock, data, len, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                keep = false;
            }
        } else if (sent < len) {
            keep = false;
        }
        xSemaphoreGive(s_log_mutex);
    }
    return keep;
}

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

    // Format outside the mutex — vsnprintf is CPU-only, no reason to hold the lock for it
    if (s_log_mutex) {
        char log_buf[256];
        int tcp_len = vsnprintf(log_buf, sizeof(log_buf), fmt, args);

        // Use timeout 0: log callbacks must never block, so drop the message on contention
        if (tcp_len > 0 && xSemaphoreTake(s_log_mutex, 0) == pdTRUE) {
            if (s_log_client_sock >= 0) {
                int send_len = MIN(tcp_len, (int)sizeof(log_buf) - 1);
                send(s_log_client_sock, log_buf, send_len, MSG_DONTWAIT);
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
 * Tear down the current TCP client: disable log forwarding, close the socket,
 * and reset the per-client line buffer. Safe to call with no client connected.
 */
static void tcp_bridge_close_client(int *client_sock, int *line_pos)
{
    tcp_bridge_set_log_client(-1);  // Disable log forwarding before closing the fd
    if (*client_sock >= 0) {
        shutdown(*client_sock, SHUT_RDWR);
        close(*client_sock);
    }
    *client_sock = -1;
    *line_pos = 0;
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
    while (!s_stop_requested) {
        // Check for new client connection (non-blocking)
        if (client_sock < 0) {
            struct sockaddr_in client_addr = {0};
            socklen_t addr_len = sizeof(client_addr);
            client_sock = accept(listen_sock,
                                 (struct sockaddr *)&client_addr,
                                 &addr_len);
            if (client_sock >= 0) {
                inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
                ESP_LOGI(TAG, "Client connected from %s", addr_str);

                // Make the client socket non-blocking so a stalled or dead peer
                // can never wedge the bridge task — which is also the only task
                // reading the UART. A full send buffer now returns immediately
                // instead of blocking the task indefinitely.
                int cflags = fcntl(client_sock, F_GETFL, 0);
                fcntl(client_sock, F_SETFL, cflags | O_NONBLOCK);

                // Enable TCP keepalive so a silently dead peer (one that never
                // sends FIN/RST) is detected and the single client slot freed,
                // without relying on the peer to send us anything.
                int ka_enable = 1;
                int ka_idle   = TCP_KEEPALIVE_IDLE_SEC;
                int ka_intvl  = TCP_KEEPALIVE_INTERVAL_SEC;
                int ka_count  = TCP_KEEPALIVE_COUNT;
                setsockopt(client_sock, SOL_SOCKET,  SO_KEEPALIVE, &ka_enable, sizeof(ka_enable));
                setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle,  sizeof(ka_idle));
                setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
                setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT,   &ka_count, sizeof(ka_count));

                // Enable log forwarding for this client
                tcp_bridge_set_log_client(client_sock);

                // Reset line buffer for new client
                line_pos = 0;

                const char *hello =
                    "Connected to pool control bus bridge.\r\n"
                    "UART bytes will be shown here in hex.\r\n"
                    "Decoded messages will also be shown.\r\n"
                    "Send hex strings (e.g., '02 00 50 FF FF 03') to transmit to the bus.\r\n\r\n";
                if (!send_to_client(client_sock, hello, strlen(hello))) {
                    ESP_LOGI(TAG, "Client dropped during greeting");
                    tcp_bridge_close_client(&client_sock, &line_pos);
                }
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
            if (!framing_add_bytes(&s_framing_buffer, uart_buf, len)) {
                ESP_LOGW(TAG, "Reassembly buffer overflow (%d + %d > %d), clearing",
                         s_framing_buffer.len, len, BUS_MESSAGE_MAX_SIZE);
                // Hand the unparseable buffer contents to the resync callback
                // (for unknown_buffer capture) before discarding them.
                if (s_config.on_resync) {
                    s_config.on_resync(TCP_BRIDGE_RESYNC_BUFFER_OVERFLOW,
                                       s_framing_buffer.buffer, s_framing_buffer.len);
                }
                framing_init(&s_framing_buffer);
                framing_add_bytes(&s_framing_buffer, uart_buf, len);
            }

            // Extract and process all complete messages
            uint8_t frame[BUS_MESSAGE_MAX_SIZE];
            int frame_len;
            framing_result_t result;
            while ((result = framing_process_next(&s_framing_buffer, frame, &frame_len)) != FRAMING_NEED_MORE_DATA) {
                if (result == FRAMING_FRAME_READY) {
                    char hexLine[3 * BUS_MESSAGE_MAX_SIZE + 3];
                    int hex_pos = 0;
                    for (int i = 0; i < frame_len; i++) {
                        if (hex_pos < (int)(sizeof(hexLine) - 3)) {
                            hex_pos += snprintf(&hexLine[hex_pos], sizeof(hexLine) - hex_pos, "%02X ", frame[i]);
                        }
                    }
                    hexLine[hex_pos] = '\0';

                    bool is_loopback = false;
                    if (s_last_tx_len > 0 && frame_len == s_last_tx_len) {
                        TickType_t time_since_tx = xTaskGetTickCount() - s_last_tx_time;
                        if (time_since_tx < pdMS_TO_TICKS(LOOPBACK_DETECTION_MS)) {
                            if (memcmp(frame, s_last_tx_msg, frame_len) == 0) {
                                is_loopback = true;
                                ESP_LOGI(TAG, "RX LOOPBACK (our TX echoed): %s", hexLine);
                                s_last_tx_len = 0;
                            }
                        }
                    }

                    if (!is_loopback) {
                        s_config.decode_message(frame, frame_len);
                    }

                    if (client_sock >= 0) {
                        hexLine[hex_pos]     = '\r';
                        hexLine[hex_pos + 1] = '\n';
                        if (!send_to_client(client_sock, hexLine, hex_pos + 2)) {
                            // Decoding above already ran; just drop the client.
                            tcp_bridge_close_client(&client_sock, &line_pos);
                        }
                    }
                } else if (s_config.on_resync) {
                    // Map each framing failure to its own resync category so the
                    // status page can show a per-type breakdown. The framing layer
                    // hands back the bytes that triggered every resync (frame/
                    // frame_len), so all types are capturable in the unknown buffer.
                    switch (result) {
                        case FRAMING_NO_START_BYTE:
                            s_config.on_resync(TCP_BRIDGE_RESYNC_NO_START, frame, frame_len); break;
                        case FRAMING_BAD_HEADER_CHECKSUM:
                            s_config.on_resync(TCP_BRIDGE_RESYNC_BAD_HEADER_CHECKSUM, frame, frame_len); break;
                        case FRAMING_BAD_FRAME_TYPE:
                            s_config.on_resync(TCP_BRIDGE_RESYNC_BAD_FRAME_TYPE, frame, frame_len); break;
                        case FRAMING_BAD_LENGTH:
                            s_config.on_resync(TCP_BRIDGE_RESYNC_BAD_LENGTH, frame, frame_len); break;
                        case FRAMING_BAD_END_BYTE:
                            s_config.on_resync(TCP_BRIDGE_RESYNC_BAD_END, frame, frame_len); break;
                        case FRAMING_BAD_DATA_CHECKSUM:
                            s_config.on_resync(TCP_BRIDGE_RESYNC_BAD_DATA_CHECKSUM, frame, frame_len); break;
                        default:
                            // FRAMING_NEED_MORE_DATA / FRAMING_FRAME_READY handled above
                            break;
                    }
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
                    if (!send_to_client(client_sock, &c, 1)) {
                        tcp_bridge_close_client(&client_sock, &line_pos);
                        break;
                    }

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
                            bool client_kept = true;
                            if (sent > 0) {
                                const char *ok_msg = "OK - sent\r\n";
                                client_kept = send_to_client(client_sock, ok_msg, strlen(ok_msg));

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
                                client_kept = send_to_client(client_sock, err_msg, strlen(err_msg));
                                s_last_tx_len = 0;  // Clear on error
                            }

                            line_pos = 0;  // Reset for next line

                            if (!client_kept) {
                                tcp_bridge_close_client(&client_sock, &line_pos);
                                break;
                            }
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
                            bool kept = send_to_client(client_sock, overflow_msg, strlen(overflow_msg));
                            line_pos = 0;
                            if (!kept) {
                                tcp_bridge_close_client(&client_sock, &line_pos);
                                break;
                            }
                        }
                    }
                }
            } else if (r == 0) {
                ESP_LOGI(TAG, "Client disconnected");
                tcp_bridge_close_client(&client_sock, &line_pos);
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "Client recv error: errno %d", errno);
                    tcp_bridge_close_client(&client_sock, &line_pos);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Cleanup on graceful stop
    tcp_bridge_set_log_client(-1);
    if (client_sock >= 0) {
        shutdown(client_sock, SHUT_RDWR);
        close(client_sock);
    }
    if (listen_sock >= 0) {
        close(listen_sock);
    }
    s_bridge_task_handle = NULL;
    if (s_stopped_sem) {
        xSemaphoreGive(s_stopped_sem);
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

    // Initialize framing buffer
    framing_init(&s_framing_buffer);

    // Create stop semaphore and reset flag
    s_stop_requested = false;
    s_stopped_sem = xSemaphoreCreateBinary();
    if (!s_stopped_sem) {
        ESP_LOGE(TAG, "Failed to create stopped semaphore");
        return ESP_ERR_NO_MEM;
    }

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

    // Signal the task to exit and wait for it to finish
    s_stop_requested = true;
    if (s_stopped_sem) {
        if (xSemaphoreTake(s_stopped_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGW(TAG, "TCP bridge task did not exit cleanly, forcing delete");
            if (s_bridge_task_handle != NULL) {
                vTaskDelete(s_bridge_task_handle);
                s_bridge_task_handle = NULL;
            }
        }
        vSemaphoreDelete(s_stopped_sem);
        s_stopped_sem = NULL;
    }
    s_stop_requested = false;

    // Restore original vprintf and clean up log mutex — safe now that the task has exited
    // and can no longer be inside tcp_bridge_vprintf holding s_log_mutex
    if (s_original_vprintf) {
        esp_log_set_vprintf(s_original_vprintf);
        s_original_vprintf = NULL;
    }
    if (s_log_mutex) {
        vSemaphoreDelete(s_log_mutex);
        s_log_mutex = NULL;
    }

    ESP_LOGI(TAG, "TCP bridge stopped");
    return ESP_OK;
}
