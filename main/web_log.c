#include "web_log.h"
#include "config.h"
#include "bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "WEB_LOG";

// ======================================================
// Ring buffer (written by the log hook, drained by the sender task)
// ======================================================

static char   s_ring[WEB_LOG_RING_SIZE];
static size_t s_ring_head;   // Next write position
static size_t s_ring_tail;   // Next read position
static size_t s_ring_used;   // Bytes currently queued
static SemaphoreHandle_t s_ring_mutex = NULL;

// Lines dropped since the last marker was queued. Atomic because the
// lock-contention path increments it without holding s_ring_mutex.
static atomic_uint s_dropped = 0;

// Caller holds s_ring_mutex and has checked there is room.
static void ring_write(const char *data, size_t len)
{
    size_t first = MIN(len, WEB_LOG_RING_SIZE - s_ring_head);
    memcpy(&s_ring[s_ring_head], data, first);
    memcpy(s_ring, data + first, len - first);
    s_ring_head = (s_ring_head + len) % WEB_LOG_RING_SIZE;
    s_ring_used += len;
}

// Caller holds s_ring_mutex. Returns the number of bytes copied out.
static size_t ring_read(char *out, size_t max)
{
    size_t len = MIN(max, s_ring_used);
    size_t first = MIN(len, WEB_LOG_RING_SIZE - s_ring_tail);
    memcpy(out, &s_ring[s_ring_tail], first);
    memcpy(out + first, s_ring, len - first);
    s_ring_tail = (s_ring_tail + len) % WEB_LOG_RING_SIZE;
    s_ring_used -= len;
    return len;
}

// Caller holds s_ring_mutex.
static void ring_reset(void)
{
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_used = 0;
    atomic_store(&s_dropped, 0);
}

// ======================================================
// Connected viewers
// ======================================================
// The fd list is only touched from the HTTP server task (the WS handler and
// the queued send work both run there), so it needs no lock. The count is
// mirrored into an atomic for the log hook and the sender task.

static httpd_handle_t s_server = NULL;
static int s_clients[WEB_LOG_MAX_CLIENTS];
static int s_num_clients = 0;
static atomic_int s_client_count = 0;

static TaskHandle_t s_sender_task = NULL;

static void remove_client_at(int i)
{
    memmove(&s_clients[i], &s_clients[i + 1],
            (size_t)(s_num_clients - i - 1) * sizeof(s_clients[0]));
    s_num_clients--;
    atomic_store(&s_client_count, s_num_clients);
}

// Drop entries whose session has closed (page reloaded/closed). httpd may
// since have reused the fd for an ordinary HTTP request, which is why the
// check is "still a WebSocket" rather than "still open".
static void prune_closed_clients(void)
{
    for (int i = s_num_clients - 1; i >= 0; i--) {
        if (httpd_ws_get_fd_info(s_server, s_clients[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
            remove_client_at(i);
        }
    }
}

static void add_client(int fd)
{
    prune_closed_clients();

    for (int i = 0; i < s_num_clients; i++) {
        if (s_clients[i] == fd) {
            return;
        }
    }

    // At the cap, the newest viewer wins: a page left open on another device
    // shouldn't lock out the one the user is looking at now.
    if (s_num_clients >= WEB_LOG_MAX_CLIENTS) {
        httpd_sess_trigger_close(s_server, s_clients[0]);
        remove_client_at(0);
    }

    // First viewer: start from an empty buffer rather than whatever was left
    // queued when the previous viewer went away.
    if (s_num_clients == 0 &&
        xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ring_reset();
        xSemaphoreGive(s_ring_mutex);
    }

    s_clients[s_num_clients++] = fd;
    atomic_store(&s_client_count, s_num_clients);
    xTaskNotifyGive(s_sender_task);
}

// ======================================================
// Sender
// ======================================================
// The sender task copies a batch out of the ring into s_frame, then hands the
// send to the HTTP server task via httpd_queue_work so it can't interleave
// with frames httpd writes itself (PONG/CLOSE). It waits for the work to
// finish before reusing s_frame.

static char   s_frame[WEB_LOG_FRAME_MAX];
static size_t s_frame_len;
static SemaphoreHandle_t s_send_done = NULL;

static void send_frame_work(void *arg)
{
    (void)arg;
    httpd_ws_frame_t frame = {
        .final   = true,
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)s_frame,
        .len     = s_frame_len,
    };

    for (int i = s_num_clients - 1; i >= 0; i--) {
        int fd = s_clients[i];
        if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            remove_client_at(i);
        } else if (httpd_ws_send_frame_async(s_server, fd, &frame) != ESP_OK) {
            httpd_sess_trigger_close(s_server, fd);
            remove_client_at(i);
        }
    }

    xSemaphoreGive(s_send_done);
}

static unsigned count_lines(const char *data, size_t len)
{
    unsigned n = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') n++;
    }
    return n;
}

// ======================================================
// Bus send (hex commands typed into the page)
// ======================================================
// The WS handler only queues the command; the sender task puts it on the bus.
// bus_send_message can wait up to a second for a quiet bus and retry on a
// collision, which must not stall the HTTP server task. The echo of a sent
// frame comes back through the bus bridge and is decoded as an RX MSG.

static QueueHandle_t s_tx_queue = NULL;
static char s_tx_cmd[WEB_LOG_RX_MAX + 1];  // Sender-task only

static void drain_ring_to_clients(void)
{
    while (atomic_load(&s_client_count) > 0) {
        if (xSemaphoreTake(s_ring_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
            break;
        }
        s_frame_len = ring_read(s_frame, sizeof(s_frame));
        xSemaphoreGive(s_ring_mutex);

        if (s_frame_len == 0) {
            break;
        }
        if (httpd_queue_work(s_server, send_frame_work, NULL) != ESP_OK) {
            // Batch lost; account for it so the page shows the gap
            atomic_fetch_add(&s_dropped, count_lines(s_frame, s_frame_len));
            break;
        }
        xSemaphoreTake(s_send_done, portMAX_DELAY);
    }
}

static void web_log_sender_task(void *arg)
{
    (void)arg;
    for (;;) {
        // Woken early by a new viewer or a queued bus command. While viewers
        // are connected, otherwise wake each flush interval so lines
        // accumulate into a batch per frame.
        TickType_t wait = atomic_load(&s_client_count) > 0
                              ? pdMS_TO_TICKS(WEB_LOG_FLUSH_MS) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);

        while (xQueueReceive(s_tx_queue, s_tx_cmd, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Web UI send: %s", s_tx_cmd);
            if (bus_send_message(s_tx_cmd) < 0) {
                ESP_LOGW(TAG, "Web UI send failed");
            }
        }

        drain_ring_to_clients();
    }
}

// ======================================================
// WebSocket endpoint
// ======================================================

// A browser always sends Origin on a WebSocket handshake, and WebSockets are
// exempt from the same-origin policy. Without this check any web page open in
// a browser on the LAN could connect, read the log and write to the bus.
// Clients that send no Origin (websocat, scripts) aren't browsers and so can't
// be driven by a third-party page; they get the same access as port 7373.
static bool origin_allowed(httpd_req_t *req)
{
    size_t origin_len = httpd_req_get_hdr_value_len(req, "Origin");
    if (origin_len == 0) {
        return true;
    }

    char origin[WEB_LOG_HOST_MAX];
    char host[WEB_LOG_HOST_MAX];
    if (origin_len >= sizeof(origin) ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    const char *origin_host = strstr(origin, "://");
    origin_host = origin_host ? origin_host + 3 : origin;
    return strcasecmp(origin_host, host) == 0;
}

static esp_err_t ws_log_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!origin_allowed(req)) {
            ESP_LOGW(TAG, "Rejected log viewer from a foreign origin");
            return ESP_FAIL;  // Closes the session
        }
        // Handshake complete — register this socket as a viewer
        add_client(httpd_req_to_sockfd(req));
        ESP_LOGI(TAG, "Log viewer connected (%d open)", s_num_clients);
        return ESP_OK;
    }

    // Frames from the page: a periodic keepalive (so httpd's LRU purge doesn't
    // pick this long-lived socket), or "TX <hex>" to send a frame to the bus.
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK || frame.len == 0) {
        return err;
    }
    char buf[WEB_LOG_RX_MAX + 1];
    if (frame.len > WEB_LOG_RX_MAX) {
        return ESP_FAIL;  // Not something our page sends; close the session
    }
    frame.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        return err;
    }
    buf[frame.len] = '\0';

    if (strncmp(buf, WEB_LOG_TX_PREFIX, strlen(WEB_LOG_TX_PREFIX)) == 0) {
        // Shift the hex down over the prefix so the queue item is the command
        memmove(buf, buf + strlen(WEB_LOG_TX_PREFIX), frame.len - strlen(WEB_LOG_TX_PREFIX) + 1);
        if (xQueueSend(s_tx_queue, buf, 0) == pdTRUE) {
            xTaskNotifyGive(s_sender_task);
        } else {
            ESP_LOGW(TAG, "Web UI send dropped: previous sends still in progress");
        }
    }
    return ESP_OK;
}

static const httpd_uri_t ws_log_uri = {
    .uri          = "/ws/log",
    .method       = HTTP_GET,
    .handler      = ws_log_handler,
    .is_websocket = true,
};

// ======================================================
// Public API
// ======================================================

esp_err_t web_log_init(void)
{
    if (s_ring_mutex) {
        return ESP_OK;
    }

    s_ring_mutex = xSemaphoreCreateMutex();
    s_send_done  = xSemaphoreCreateBinary();
    s_tx_queue   = xQueueCreate(WEB_LOG_TX_QUEUE_LEN, sizeof(s_tx_cmd));
    if (!s_ring_mutex || !s_send_done || !s_tx_queue) {
        ESP_LOGE(TAG, "Failed to create semaphores/queue");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(web_log_sender_task, "web_log", WEB_LOG_TASK_STACK,
                    NULL, WEB_LOG_TASK_PRIORITY, &s_sender_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sender task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t web_log_register(httpd_handle_t server)
{
    s_server = server;
    return httpd_register_uri_handler(server, &ws_log_uri);
}

bool web_log_active(void)
{
    return atomic_load(&s_client_count) > 0;
}

void web_log_append(const char *line, int len)
{
    if (len <= 0 || !s_ring_mutex) {
        return;
    }

    // Log hooks must never block: drop the line on contention
    if (xSemaphoreTake(s_ring_mutex, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1);
        return;
    }

    size_t room = WEB_LOG_RING_SIZE - s_ring_used;
    unsigned dropped = atomic_load(&s_dropped);

    // Mark the gap where it happened, before the first line that fits again.
    // While the marker itself doesn't fit, keep dropping so order is kept.
    if (dropped > 0) {
        char marker[48];
        int mlen = snprintf(marker, sizeof(marker), "--- %u lines dropped ---\n", dropped);
        if ((size_t)mlen + (size_t)len <= room) {
            ring_write(marker, (size_t)mlen);
            atomic_fetch_sub(&s_dropped, dropped);
            room -= (size_t)mlen;
            dropped = 0;
        }
    }

    if (dropped == 0 && (size_t)len <= room) {
        ring_write(line, (size_t)len);
    } else {
        atomic_fetch_add(&s_dropped, 1);
    }

    xSemaphoreGive(s_ring_mutex);
}
