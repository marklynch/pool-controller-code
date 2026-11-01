#include "firmware_update.h"
#include "config.h"
#include "mqtt_poolclient.h"
#include "usage_report.h"
#include "wifi_provisioning.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "FW_UPDATE";

// Cached status, protected by s_status_lock.
static fw_update_status_t s_status;
static SemaphoreHandle_t s_status_lock;

// Serializes network operations (a periodic check must not race a manual
// check or an install, and two installs must not run at once). Binary
// semaphore, not a mutex: firmware_update_start_install takes it and hands
// ownership to install_task, which releases it from a different task —
// a mutex would assert in xTaskPriorityDisinherit on that cross-task give.
static SemaphoreHandle_t s_op_lock;

// Release tag the pending install should fetch. Written by
// firmware_update_start_install while it holds s_op_lock, read by install_task.
static char s_install_tag[FW_UPDATE_VERSION_LEN];

#define FW_UPDATE_NVS_NAMESPACE      "fw_update"
#define FW_UPDATE_NVS_KEY_AUTO_CHECK "auto_check"

// Written from the httpd task (firmware_update_set_auto_check) and read from
// the check task. Unsynchronised: an aligned bool is read and written
// atomically on this core, and a check that catches the old value is at most
// one check either side of the switch.
static bool s_auto_check = FW_UPDATE_AUTO_CHECK_DEFAULT_ON;

// ======================================================
// Status helpers
// ======================================================

static void status_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(s_status.installed_version, app->version, sizeof(s_status.installed_version) - 1);
    s_status.state = FW_UPDATE_STATE_IDLE;
}

static void status_set_state(fw_update_state_t state)
{
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.state = state;
        xSemaphoreGive(s_status_lock);
    }
}

static void status_set_error(const char *msg)
{
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.state = FW_UPDATE_STATE_ERROR;
        strncpy(s_status.last_error, msg, sizeof(s_status.last_error) - 1);
        s_status.last_error[sizeof(s_status.last_error) - 1] = '\0';
        xSemaphoreGive(s_status_lock);
    }
    ESP_LOGW(TAG, "%s", msg);
}

void firmware_update_get_status(fw_update_status_t *out)
{
    if (!s_status_lock) {          // Not initialized yet
        memset(out, 0, sizeof(*out));
        return;
    }
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_status_lock);
    }
}

// ======================================================
// Version comparison (major.minor.patch, tolerant of a leading 'v' and any
// trailing "-<n>-g<hash>[-dirty]" suffix produced by `git describe`)
// ======================================================

void firmware_update_parse_version(const char *s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;
    int idx = 0;
    while (*s && idx < 3) {
        if (!isdigit((unsigned char)*s)) break;
        int v = 0;
        while (isdigit((unsigned char)*s)) {
            v = v * 10 + (*s - '0');
            s++;
        }
        out[idx++] = v;
        if (*s == '.') s++;
        else break;
    }
}

// ======================================================
// Bootloader version
// ======================================================

uint32_t firmware_update_bootloader_version(void)
{
    // The bootloader can't change while the app runs, so read flash once
    static bool s_read;
    static uint32_t s_version;
    if (!s_read) {
        esp_bootloader_desc_t desc;
        if (esp_ota_get_bootloader_description(NULL, &desc) == ESP_OK) {
            s_version = desc.version;
        }
        s_read = true;
    }
    return s_version;
}

bool firmware_update_bootloader_has_rollback(void)
{
    return firmware_update_bootloader_version() >= BOOTLOADER_ROLLBACK_MIN_VERSION;
}

// Returns <0 if a<b, 0 if equal, >0 if a>b (by major.minor.patch).
static int semver_cmp(const char *a, const char *b)
{
    int va[3], vb[3];
    firmware_update_parse_version(a, va);
    firmware_update_parse_version(b, vb);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

// ======================================================
// MQTT update-entity state
// ======================================================

void firmware_update_publish_mqtt_state(void)
{
    if (!s_status_lock) return;   // Not initialized yet

    fw_update_status_t st;
    firmware_update_get_status(&st);

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[96];
    snprintf(topic, sizeof(topic), "pool/%s/update/state", device_id);

    // Home Assistant "update" platform reads installed_version/latest_version
    // and (optionally) in_progress/update_percentage from a JSON state payload.
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "installed_version", st.installed_version);
    // HA compares installed/latest itself and treats an uncomparable pair
    // (e.g. a git-describe dev build like v1.7.0-11-g<hash>-dirty vs v1.7.0)
    // as "update available". Our semver_cmp already made that call, so only
    // publish the real tag when an update is actually available; otherwise
    // mirror installed so HA shows "up to date" (also covers the not-yet-
    // checked case, where latest is still empty).
    cJSON_AddStringToObject(root, "latest_version",
                            (st.update_available && st.latest_version[0])
                                ? st.latest_version : st.installed_version);
    if (st.release_url[0]) {
        cJSON_AddStringToObject(root, "release_url", st.release_url);
    }
    cJSON_AddStringToObject(root, "title", "Pool Controller Firmware");

    // SUCCESS (flashed, reboot imminent) still counts as in progress so HA
    // keeps showing "installing" across the reboot instead of reverting to
    // "Update available"; the retained message holds until the new firmware's
    // boot-time publish delivers the new installed_version. A null percentage
    // renders as an indeterminate bar.
    bool in_progress = (st.state == FW_UPDATE_STATE_DOWNLOADING ||
                        st.state == FW_UPDATE_STATE_SUCCESS);
    cJSON_AddBoolToObject(root, "in_progress", in_progress);
    if (st.state == FW_UPDATE_STATE_DOWNLOADING) {
        cJSON_AddNumberToObject(root, "update_percentage", st.progress_pct);
    } else {
        cJSON_AddNullToObject(root, "update_percentage");
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        mqtt_publish(topic, json, 1, true);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

// ======================================================
// GitHub recent-releases lookup
//
// Rather than hitting the rate-limited JSON API, we GET the Atom feed at
// ".../releases.atom", which lists recent releases newest-first without
// authentication. Each entry carries a link of the form
// href="https://github.com/<owner>/<repo>/releases/tag/<tag>", so we look for
// "/releases/tag/" and read the tag that follows.
//
// The feed embeds each release's (potentially large) rendered notes, so the
// tag links can be spread across tens of KB. We therefore parse the response
// as it streams with a small byte-level state machine — no full-body buffer —
// which also transparently handles a needle/tag split across chunk boundaries.
// ======================================================

static const char FEED_NEEDLE[] = "/releases/tag/";
#define FEED_NEEDLE_LEN (sizeof(FEED_NEEDLE) - 1)
static const char FEED_ENTRY[] = "<entry";
#define FEED_ENTRY_LEN (sizeof(FEED_ENTRY) - 1)

typedef struct {
    char (*tags)[FW_UPDATE_VERSION_LEN];
    int max;
    int count;
    int needle_pos;                 // matched chars of FEED_NEEDLE so far
    int entry_pos;                  // matched chars of FEED_ENTRY so far
    bool armed;                     // saw "<entry"; next tag link is the entry's own
    bool collecting;                // currently reading the tag after a match
    char tag[FW_UPDATE_VERSION_LEN];
    size_t tag_len;
} feed_parse_ctx_t;

static bool is_tag_delim(char c)
{
    return c == '"' || c == '<' || c == '/' || c == '\'' ||
           c == '?' || c == '#' || isspace((unsigned char)c);
}

// Store the collected tag, but only if it is the first "/releases/tag/" link
// of an entry (armed) — this ignores any tag URLs embedded in a release's
// notes, which would otherwise corrupt the ordered list.
static void feed_store_tag(feed_parse_ctx_t *ctx)
{
    if (ctx->armed && ctx->tag_len > 0 && ctx->count < ctx->max) {
        ctx->tag[ctx->tag_len] = '\0';
        bool dup = false;
        for (int k = 0; k < ctx->count; k++) {
            if (strcmp(ctx->tags[k], ctx->tag) == 0) { dup = true; break; }
        }
        if (!dup) {
            strncpy(ctx->tags[ctx->count], ctx->tag, FW_UPDATE_VERSION_LEN - 1);
            ctx->tags[ctx->count][FW_UPDATE_VERSION_LEN - 1] = '\0';
            ctx->count++;
        }
    }
    ctx->armed = false;             // one tag per entry
    ctx->collecting = false;
    ctx->tag_len = 0;
    ctx->needle_pos = 0;
}

static void feed_consume(feed_parse_ctx_t *ctx, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        char c = data[i];

        // Track entry boundaries so only each entry's own tag link is taken.
        if (c == FEED_ENTRY[ctx->entry_pos]) {
            ctx->entry_pos++;
            if (ctx->entry_pos == (int)FEED_ENTRY_LEN) {
                ctx->armed = true;
                ctx->entry_pos = 0;
            }
        } else {
            ctx->entry_pos = (c == FEED_ENTRY[0]) ? 1 : 0;
        }

        if (ctx->collecting) {
            if (!is_tag_delim(c) && ctx->tag_len < FW_UPDATE_VERSION_LEN - 1) {
                ctx->tag[ctx->tag_len++] = c;
                continue;
            }
            // Delimiter (or overflow): finish this tag, then let the delimiter
            // fall through to the needle matcher (a trailing '/' can begin the
            // next "/releases/tag/").
            feed_store_tag(ctx);
        }

        if (ctx->count >= ctx->max) continue;

        if (c == FEED_NEEDLE[ctx->needle_pos]) {
            ctx->needle_pos++;
            if (ctx->needle_pos == (int)FEED_NEEDLE_LEN) {
                ctx->collecting = true;
                ctx->tag_len = 0;
                ctx->needle_pos = 0;
            }
        } else {
            ctx->needle_pos = (c == FEED_NEEDLE[0]) ? 1 : 0;
        }
    }
}

static esp_err_t feed_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        feed_parse_ctx_t *ctx = (feed_parse_ctx_t *)evt->user_data;
        if (ctx) feed_consume(ctx, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

// Fetch the releases Atom feed and parse up to `max` newest tags into `tags`.
static esp_err_t query_recent_releases(char tags[][FW_UPDATE_VERSION_LEN],
                                       int max, int *count)
{
    feed_parse_ctx_t ctx = {
        .tags = tags,
        .max = max,
        .count = 0,
        .needle_pos = 0,
        .collecting = false,
        .tag_len = 0,
    };

    char url[128];
    snprintf(url, sizeof(url), "https://github.com/%s/%s/releases.atom",
             FW_UPDATE_GITHUB_OWNER, FW_UPDATE_GITHUB_REPO);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = FW_UPDATE_HTTP_TIMEOUT_MS,
        .event_handler = feed_http_event,
        .user_data = &ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "User-Agent", "pool-controller-esp32");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    // If the body ended mid-tag (feed always closes with markup, so unlikely),
    // still capture what we collected.
    if (ctx.collecting) feed_store_tag(&ctx);
    *count = ctx.count;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "release feed request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "unexpected release feed response (HTTP %d)", status);
        return ESP_FAIL;
    }
    return (*count > 0) ? ESP_OK : ESP_FAIL;
}

// Query GitHub for the recent releases and update the cached status (also
// publishes the MQTT update state). Runs only in the check task.
static void run_check(void)
{
    if (xSemaphoreTake(s_op_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "check skipped: an update operation is already running");
        return;
    }

    status_set_state(FW_UPDATE_STATE_CHECKING);

    char tags[FW_UPDATE_MAX_VERSIONS][FW_UPDATE_VERSION_LEN];
    int count = 0;
    esp_err_t err = query_recent_releases(tags, FW_UPDATE_MAX_VERSIONS, &count);

    if (err != ESP_OK) {
        status_set_error("GitHub release check failed");
        xSemaphoreGive(s_op_lock);
        firmware_update_publish_mqtt_state();
        return;
    }

    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.version_count = count;
        for (int i = 0; i < count; i++) {
            strncpy(s_status.versions[i], tags[i], FW_UPDATE_VERSION_LEN - 1);
            s_status.versions[i][FW_UPDATE_VERSION_LEN - 1] = '\0';
        }
        // versions[0] is the newest release.
        strncpy(s_status.latest_version, tags[0], sizeof(s_status.latest_version) - 1);
        s_status.latest_version[sizeof(s_status.latest_version) - 1] = '\0';
        snprintf(s_status.release_url, sizeof(s_status.release_url),
                 "https://github.com/%s/%s/releases/tag/%s",
                 FW_UPDATE_GITHUB_OWNER, FW_UPDATE_GITHUB_REPO, s_status.latest_version);
        s_status.update_available =
            (semver_cmp(s_status.installed_version, s_status.latest_version) < 0);
        s_status.checked = true;
        s_status.last_error[0] = '\0';
        s_status.state = FW_UPDATE_STATE_IDLE;
        ESP_LOGI(TAG, "installed=%s latest=%s (%d releases) update_available=%d",
                 s_status.installed_version, s_status.latest_version,
                 count, s_status.update_available);
        xSemaphoreGive(s_status_lock);
    }

    xSemaphoreGive(s_op_lock);
    firmware_update_publish_mqtt_state();
}

// ======================================================
// OTA install from the latest GitHub release asset
// ======================================================

static void set_progress(int pct)
{
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.progress_pct = pct;
        xSemaphoreGive(s_status_lock);
    }
}

static void install_task(void *arg)
{
    // s_op_lock is already held by the caller (firmware_update_start_install),
    // which also populated s_install_tag.
    char tag[FW_UPDATE_VERSION_LEN];
    strncpy(tag, s_install_tag, sizeof(tag) - 1);
    tag[sizeof(tag) - 1] = '\0';

    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.progress_pct = 0;
        s_status.state = FW_UPDATE_STATE_DOWNLOADING;
        xSemaphoreGive(s_status_lock);
    }

    char asset_url[224];
    snprintf(asset_url, sizeof(asset_url),
             "https://github.com/%s/%s/releases/download/%s/%s%s.bin",
             FW_UPDATE_GITHUB_OWNER, FW_UPDATE_GITHUB_REPO, tag,
             FW_UPDATE_ASSET_PREFIX, tag);
    ESP_LOGI(TAG, "Starting OTA from %s", asset_url);

    firmware_update_publish_mqtt_state();

    esp_http_client_config_t http_cfg = {
        .url = asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = FW_UPDATE_OTA_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size = FW_UPDATE_HTTP_BUF_SIZE,
        .buffer_size_tx = FW_UPDATE_HTTP_BUF_SIZE,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota);
    if (err != ESP_OK || ota == NULL) {
        status_set_error("Download failed to start");
        firmware_update_publish_mqtt_state();
        xSemaphoreGive(s_op_lock);
        vTaskDelete(NULL);
        return;
    }

    int total = esp_https_ota_get_image_size(ota);
    int last_published = -10;

    while (1) {
        err = esp_https_ota_perform(ota);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int read = esp_https_ota_get_image_len_read(ota);
        int pct = (total > 0) ? (int)((int64_t)read * 100 / total) : 0;
        set_progress(pct);
        if (pct - last_published >= 5) {
            last_published = pct;
            ESP_LOGI(TAG, "OTA progress %d%% (%d/%d bytes)", pct, read, total);
            firmware_update_publish_mqtt_state();
        }
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota)) {
        err = esp_https_ota_finish(ota);
        if (err == ESP_OK) {
            set_progress(100);
            status_set_state(FW_UPDATE_STATE_SUCCESS);
            firmware_update_publish_mqtt_state();
            ESP_LOGI(TAG, "OTA successful, rebooting into new firmware");
            xSemaphoreGive(s_op_lock);
            vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
            esp_restart();
            return;  // not reached
        }
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            status_set_error("Downloaded image failed validation");
        } else {
            status_set_error("Failed to finalize update");
        }
    } else {
        esp_https_ota_abort(ota);
        status_set_error("Download failed or incomplete");
    }

    firmware_update_publish_mqtt_state();
    xSemaphoreGive(s_op_lock);
    vTaskDelete(NULL);
}

esp_err_t firmware_update_start_install(const char *tag)
{
    // Resolve the requested tag to a known release. An empty/NULL tag means
    // "latest"; any explicit tag must be one we discovered in the last check,
    // so the web/MQTT callers can't point the OTA at an arbitrary URL.
    char target[FW_UPDATE_VERSION_LEN] = {0};
    bool known = false;
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        if (!tag || tag[0] == '\0') {
            strncpy(target, s_status.latest_version, sizeof(target) - 1);
        } else {
            strncpy(target, tag, sizeof(target) - 1);
        }
        target[sizeof(target) - 1] = '\0';
        for (int i = 0; i < s_status.version_count; i++) {
            if (strcmp(s_status.versions[i], target) == 0) { known = true; break; }
        }
        xSemaphoreGive(s_status_lock);
    }

    if (target[0] == '\0') {
        ESP_LOGW(TAG, "install requested but no release is known yet");
        return ESP_ERR_INVALID_STATE;
    }
    if (!known) {
        ESP_LOGW(TAG, "install requested for unknown release '%s'", target);
        return ESP_ERR_NOT_FOUND;
    }

    if (xSemaphoreTake(s_op_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "install skipped: an update operation is already running");
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(s_install_tag, target, sizeof(s_install_tag) - 1);
    s_install_tag[sizeof(s_install_tag) - 1] = '\0';
    ESP_LOGI(TAG, "Installing release %s", s_install_tag);

    // install_task owns s_op_lock from here and releases it (or reboots).
    if (xTaskCreate(install_task, "fw_ota", FW_UPDATE_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        status_set_error("Could not start update task");
        xSemaphoreGive(s_op_lock);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ======================================================
// Background check task
// ======================================================

static TaskHandle_t s_check_task;

// Notification bits for the check task. CHECK asks for a release check, even
// with automatic checks off; WAKE only runs the task's other work (the usage
// report) now instead of at the next interval.
#define CHECK_TASK_NOTIFY_CHECK  (1u << 0)
#define CHECK_TASK_NOTIFY_WAKE   (1u << 1)

// The usage ping rides on this task's schedule and TLS-sized stack, but must
// never compete with an install for heap or bandwidth.
static bool install_in_progress(void)
{
    fw_update_status_t st;
    firmware_update_get_status(&st);
    return st.state == FW_UPDATE_STATE_DOWNLOADING || st.state == FW_UPDATE_STATE_SUCCESS;
}

static void check_task(void *arg)
{
    // Notification-based waits so firmware_update_request_check() and
    // firmware_update_wake() can cut the interval short. A wait that times
    // out returns no bits: the periodic run, which checks only if automatic
    // checks are on. Any early wake restarts the interval.
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(FW_UPDATE_STARTUP_DELAY_MS));
    while (1) {
        bool timed_out = (bits == 0);
        if ((bits & CHECK_TASK_NOTIFY_CHECK) || (timed_out && s_auto_check)) {
            run_check();
        }
        if (wifi_is_connected() && !install_in_progress()) {
            usage_report_run();
        }
        bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(FW_UPDATE_CHECK_INTERVAL_MS));
    }
}

void firmware_update_wake(void)
{
    if (s_check_task) {
        xTaskNotify(s_check_task, CHECK_TASK_NOTIFY_WAKE, eSetBits);
    }
}

bool firmware_update_get_auto_check(void)
{
    return s_auto_check;
}

void firmware_update_set_auto_check(bool enabled)
{
    s_auto_check = enabled;
    ESP_LOGI(TAG, "Automatic release checks %s", enabled ? "enabled" : "disabled");

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(FW_UPDATE_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        // The setting still applies for this boot; only persistence is lost.
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs_handle, FW_UPDATE_NVS_KEY_AUTO_CHECK, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store automatic check setting: %s", esp_err_to_name(err));
    }
    nvs_close(nvs_handle);
}

static void load_auto_check(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(FW_UPDATE_NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return;   // Nothing stored yet: keep the default
    }
    uint8_t stored;
    if (nvs_get_u8(nvs_handle, FW_UPDATE_NVS_KEY_AUTO_CHECK, &stored) == ESP_OK) {
        s_auto_check = (stored != 0);
    }
    nvs_close(nvs_handle);
}

void firmware_update_request_check(void)
{
    if (!s_check_task) {
        return;
    }
    // Report "checking" straight away rather than when the task wakes, so a
    // caller polling the status (the web page) never reads the previous
    // result, takes it as the answer and stops polling. Never over an
    // install: its state drives the progress display, and the check it would
    // announce is skipped while the install holds s_op_lock.
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        if (s_status.state == FW_UPDATE_STATE_IDLE || s_status.state == FW_UPDATE_STATE_ERROR) {
            s_status.state = FW_UPDATE_STATE_CHECKING;
        }
        xSemaphoreGive(s_status_lock);
    }
    xTaskNotify(s_check_task, CHECK_TASK_NOTIFY_CHECK, eSetBits);
}

void firmware_update_init(void)
{
    s_status_lock = xSemaphoreCreateMutex();
    s_op_lock = xSemaphoreCreateBinary();
    if (!s_status_lock || !s_op_lock) {
        ESP_LOGE(TAG, "Failed to create firmware update locks");
        return;
    }
    xSemaphoreGive(s_op_lock);   // binary semaphores start empty; mark available
    status_init();
    load_auto_check();

    if (xTaskCreate(check_task, "fw_check", FW_UPDATE_TASK_STACK, NULL, 3, &s_check_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start firmware check task");
    } else {
        ESP_LOGI(TAG, "Firmware update checker started (installed %s, automatic checks %s)",
                 s_status.installed_version, s_auto_check ? "on" : "off");
    }
}
