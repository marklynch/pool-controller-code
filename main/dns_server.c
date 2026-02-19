#include "dns_server.h"
#include "config.h"

#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "DNS_SERVER";

// DNS header structure
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} __attribute__((packed)) dns_header_t;

static int s_dns_socket = -1;
static TaskHandle_t s_dns_task_handle = NULL;

// Convert IP string to 32-bit integer (network byte order)
static uint32_t ip_string_to_uint32(const char *ip_str)
{
    uint8_t ip[4];
    sscanf(ip_str, "%hhu.%hhu.%hhu.%hhu", &ip[0], &ip[1], &ip[2], &ip[3]);
    return (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3];
}

// Build DNS response packet
static int build_dns_response(const uint8_t *query, int query_len, uint8_t *response, const char *ip_addr)
{
    if (query_len < sizeof(dns_header_t)) {
        return 0;
    }

    // Copy query to response
    memcpy(response, query, query_len);

    // Modify header for response
    dns_header_t *header = (dns_header_t *)response;
    header->flags = htons(0x8180);  // Standard query response, no error
    header->answers = htons(1);     // One answer
    header->authority = 0;
    header->additional = 0;

    // Build answer section at end of query
    int pos = query_len;

    // Name pointer (compression) - points to question name
    response[pos++] = 0xC0;  // Pointer flag
    response[pos++] = 0x0C;  // Offset to question name (after header)

    // Type: A record
    response[pos++] = 0x00;
    response[pos++] = 0x01;

    // Class: IN
    response[pos++] = 0x00;
    response[pos++] = 0x01;

    // TTL: 60 seconds
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x00;
    response[pos++] = 0x3C;

    // Data length: 4 bytes (IPv4)
    response[pos++] = 0x00;
    response[pos++] = 0x04;

    // IP address
    uint32_t ip = ip_string_to_uint32(ip_addr);
    response[pos++] = (ip >> 24) & 0xFF;
    response[pos++] = (ip >> 16) & 0xFF;
    response[pos++] = (ip >> 8) & 0xFF;
    response[pos++] = ip & 0xFF;

    return pos;
}

// DNS server task
static void dns_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    uint8_t rx_buffer[DNS_MAX_PACKET_SIZE];
    uint8_t tx_buffer[DNS_MAX_PACKET_SIZE];

    // Create UDP socket
    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Set socket to non-blocking mode with timeout
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(s_dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Bind to DNS port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);

    if (bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: errno %d", errno);
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);

    // Main DNS server loop
    while (1) {
        // Receive DNS query
        int len = recvfrom(s_dns_socket, rx_buffer, sizeof(rx_buffer), 0,
                          (struct sockaddr *)&client_addr, &client_addr_len);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout, check if we should continue
                continue;
            }
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }

        if (len < sizeof(dns_header_t)) {
            ESP_LOGW(TAG, "Received malformed DNS packet (too short)");
            continue;
        }

        // Build response
        int response_len = build_dns_response(rx_buffer, len, tx_buffer, WIFI_PROV_SOFTAP_IP);
        if (response_len > 0) {
            // Send response
            int sent = sendto(s_dns_socket, tx_buffer, response_len, 0,
                            (struct sockaddr *)&client_addr, client_addr_len);
            if (sent < 0) {
                ESP_LOGW(TAG, "Failed to send DNS response: errno %d", errno);
            } else {
                ESP_LOGD(TAG, "Sent DNS response (%d bytes) -> %s", sent, WIFI_PROV_SOFTAP_IP);
            }
        }
    }

    // Cleanup
    close(s_dns_socket);
    s_dns_socket = -1;
    s_dns_task_handle = NULL;
    ESP_LOGI(TAG, "DNS server stopped");
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(void)
{
    if (s_dns_task_handle != NULL) {
        ESP_LOGW(TAG, "DNS server already running");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(dns_server_task, "dns_server",
                                 DNS_TASK_STACK_SIZE, NULL,
                                 DNS_TASK_PRIORITY, &s_dns_task_handle);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS server task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void dns_server_stop(void)
{
    if (s_dns_task_handle != NULL) {
        // Close socket to trigger task exit
        if (s_dns_socket >= 0) {
            close(s_dns_socket);
            s_dns_socket = -1;
        }

        // Wait for task to finish
        vTaskDelay(pdMS_TO_TICKS(100));

        if (s_dns_task_handle != NULL) {
            vTaskDelete(s_dns_task_handle);
            s_dns_task_handle = NULL;
        }

        ESP_LOGI(TAG, "DNS server stopped");
    }
}
