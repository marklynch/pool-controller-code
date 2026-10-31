#ifndef WEB_LOG_H
#define WEB_LOG_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Live log feed for the web UI's Debug Log page.
 *
 * Log lines are copied into a fixed ring buffer by the log vprintf hook and
 * pushed to connected browsers over a WebSocket (/ws/log) by a small sender
 * task. The device never stores the capture itself — the page keeps it.
 *
 * When no viewer is connected, web_log_active() returns false and the hook
 * skips this module entirely, so the feature costs nothing when unused.
 */

/** Create the ring-buffer lock and the sender task. Call once at startup,
 *  before the HTTP server registers its handlers. */
esp_err_t web_log_init(void);

/** Register the /ws/log WebSocket endpoint on the given server. */
esp_err_t web_log_register(httpd_handle_t server);

/** True while at least one log viewer is connected. Cheap; safe from any task. */
bool web_log_active(void);

/**
 * Queue an already-formatted log line for the connected viewers.
 * Never blocks: on lock contention or a full buffer the line is dropped and
 * counted, and a "--- N lines dropped ---" marker is queued in its place once
 * there is room again. Safe to call from any task (not from an ISR).
 */
void web_log_append(const char *line, int len);

#endif // WEB_LOG_H
