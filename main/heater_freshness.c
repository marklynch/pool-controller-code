#include "heater_freshness.h"
#include "heater_freshness_logic.h"
#include "config.h"
#include "pool_state.h"
#include "mqtt_publish.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "HEATER_FRESHNESS";

static void freshness_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEATER_FRESHNESS_POLL_MS));

        pool_state_t snapshot;
        uint32_t expired_mask = 0;
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (s_pool_state_mutex &&
            xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            expired_mask = heater_freshness_expire(
                &s_pool_state, now_ms, HEATER_RESPONSE_TIMEOUT_MS);
            snapshot = s_pool_state;
            xSemaphoreGive(s_pool_state_mutex);
        }

        for (int i = 0; i < MAX_HEATERS; i++) {
            if ((expired_mask & (1U << i)) == 0) continue;
            ESP_LOGW(TAG, "Heater %d telemetry expired after %u seconds",
                     i + 1, HEATER_RESPONSE_TIMEOUT_MS / 1000);
            mqtt_publish_heater_telemetry(&snapshot, i, true);
        }
    }
}

void heater_freshness_start(void)
{
    xTaskCreate(freshness_task, "heater_freshness", 3072, NULL, 3, NULL);
}
