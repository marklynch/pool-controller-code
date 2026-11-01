#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

// ======================================================
// GitHub-backed firmware update
//
// Periodically checks the project's GitHub "latest release" and compares its
// version tag against the running firmware. When a newer release is available
// it can be installed over-the-air by pulling the release's update binary
// directly from GitHub (esp_https_ota), triggered either from the web UI or
// the Home Assistant "update" entity.
// ======================================================

// Overall state of the update subsystem. Reported to the web UI and mirrored
// into the Home Assistant update entity (in_progress / percentage).
typedef enum {
    FW_UPDATE_STATE_IDLE = 0,   // Nothing happening; latest info may or may not be known
    FW_UPDATE_STATE_CHECKING,   // Querying GitHub for the latest release
    FW_UPDATE_STATE_DOWNLOADING,// OTA in progress (downloading + writing)
    FW_UPDATE_STATE_SUCCESS,    // OTA finished successfully; reboot imminent
    FW_UPDATE_STATE_ERROR,      // Last check or install failed (see last_error)
} fw_update_state_t;

// Snapshot of the update subsystem, safe to read from any task.
typedef struct {
    fw_update_state_t state;
    char installed_version[FW_UPDATE_VERSION_LEN]; // Running firmware version (esp_app_desc)
    char latest_version[FW_UPDATE_VERSION_LEN];    // Latest GitHub release tag, "" if unknown
    // Most-recent release tags, newest first (versions[0] == latest_version).
    char versions[FW_UPDATE_MAX_VERSIONS][FW_UPDATE_VERSION_LEN];
    int  version_count;         // Number of valid entries in versions[]
    char release_url[160];      // URL of the latest release page, "" if unknown
    bool checked;               // True once at least one check has completed
    bool update_available;      // latest_version is newer than installed_version
    int  progress_pct;          // 0-100 during FW_UPDATE_STATE_DOWNLOADING
    char last_error[80];        // Human-readable error for the last failure
} fw_update_status_t;

// Start the background check task. Safe to call once, after WiFi is up.
void firmware_update_init(void);

// Copy the current status snapshot into `out`.
void firmware_update_get_status(fw_update_status_t *out);

// Ask the background check task to run a check as soon as possible instead
// of waiting out its interval. Non-blocking, safe to call from any task
// (the MQTT event handler, a web handler) — the check runs in the check task.
// The status reads FW_UPDATE_STATE_CHECKING from the moment this returns
// until the check finishes, unless an install is in progress.
void firmware_update_request_check(void);

// Wake the check task to send a pending usage-report deletion now, without a
// release check or a usage ping — so opting out of usage reporting deletes
// the server record promptly. Non-blocking, safe to call from any task.
void firmware_update_wake(void);

// Whether the check task queries GitHub on its own schedule (shortly after
// boot, then every FW_UPDATE_CHECK_INTERVAL_MS). Off, only requested checks
// run; the task keeps its schedule for the usage ping. Stored in NVS.
bool firmware_update_get_auto_check(void);
void firmware_update_set_auto_check(bool enabled);

// Kick off an OTA install in a background task. `tag` selects which release to
// install and must be one of the known recent versions (see fw_update_status_t
// versions[]); pass NULL or "" to install the latest. Returns ESP_OK if the
// install task was started, ESP_ERR_INVALID_STATE if an install is already
// running, or ESP_ERR_NOT_FOUND if `tag` is not a known release.
esp_err_t firmware_update_start_install(const char *tag);

// Publish the current update state to the Home Assistant MQTT update entity.
// Called internally after checks/installs; also called on MQTT (re)connect so
// the retained state matches the device.
void firmware_update_publish_mqtt_state(void);

// Version of the bootloader in flash (its CONFIG_BOOTLOADER_PROJECT_VER), or 0
// if it has no description block (built before IDF 5.1). OTA never replaces
// the bootloader, so this can lag the app until the next USB flash.
uint32_t firmware_update_bootloader_version(void);

// True if the bootloader in flash supports rolling back a failed update.
bool firmware_update_bootloader_has_rollback(void);

// Parse a version string into {major, minor, patch}. Tolerates a leading 'v'
// and any trailing "-<n>-g<hash>[-dirty]" suffix from `git describe`; missing
// or unparseable parts are 0 (e.g. an untagged build's bare hash is 0.0.0).
void firmware_update_parse_version(const char *s, int out[3]);

#endif // FIRMWARE_UPDATE_H
