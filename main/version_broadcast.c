#include "version_broadcast.h"
#include "bus.h"
#include "firmware_version.h"
#include "pool_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"

static const char *TAG = "VER_BCAST";

// First announcement a few seconds after startup, clear of the register
// requester's early Mode read
#define VERSION_BROADCAST_STARTUP_DELAY_MS  5000
// Other devices repeat their 0x0A about every 28-60 s (PROTOCOL.md 0x0A)
#define VERSION_BROADCAST_INTERVAL_MS       60000

static uint8_t clamp_u8(int v)
{
    return v < 0 ? 0 : (v > 0xFF ? 0xFF : (uint8_t)v);
}

static void version_broadcast_task(void *arg)
{
    (void)arg;

    const esp_app_desc_t *app = esp_app_get_description();
    fw_version_t ver;
    firmware_version_parse(app->version, &ver);
    uint8_t major = clamp_u8(ver.major);
    uint8_t minor = clamp_u8(ver.minor);

    // 02 AC 1D FF FF 80 00 0A 0E <hdr_ck> <major> <minor> <data_ck> 03
    uint8_t frame[14] = {
        0x02, SELF_DEVICE_ID_HI, SELF_DEVICE_ID_LO, 0xFF, 0xFF, 0x80, 0x00,
        0x0A, 0x0E, 0x00, major, minor, 0x00, 0x03,
    };
    for (int i = 0; i < 9; i++) frame[9] += frame[i];
    frame[12] = major + minor;

    ESP_LOGI(TAG, "Announcing firmware %s as version %d.%d", app->version, major, minor);

    vTaskDelay(pdMS_TO_TICKS(VERSION_BROADCAST_STARTUP_DELAY_MS));
    for (;;) {
        bus_send_bytes(frame, sizeof(frame));
        vTaskDelay(pdMS_TO_TICKS(VERSION_BROADCAST_INTERVAL_MS));
    }
}

void version_broadcast_start(void)
{
    // The stack needs room for bus_send_bytes' hex log buffer plus the
    // logging path (vprintf and the TCP log-forward hook)
    xTaskCreate(version_broadcast_task, "ver_bcast", 3072, NULL, 2, NULL);
}
