#include "register_requester.h"
#include "bus.h"
#include "pool_state.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "REG_REQ";
static TaskHandle_t s_task_handle = NULL;

// Wait this long at startup before checking — gives the gateway time to announce itself
#define STARTUP_DELAY_MS     55000
// Delay between individual requests in a sequence
#define REQUEST_INTERVAL_MS  250
// How long to wait before re-checking if data is still missing
#define RETRY_INTERVAL_MS    60000

// Header bytes for a CMD 0x39 register read request originating from the Internet Gateway.
// Header checksum 0xB7 = (02+00+F0+FF+FF+80+00+39+0E) & 0xFF, constant for all requests.
#define REQUEST_HEADER "02 00 F0 FF FF 80 00 39 0E B7"

static void send_request(uint8_t reg_id, uint8_t slot, const char *description)
{
    uint8_t checksum = reg_id + slot;
    char msg[48];
    snprintf(msg, sizeof(msg), "%s %02X %02X %02X 03", REQUEST_HEADER, reg_id, slot, checksum);
    ESP_LOGI(TAG, "Requesting %s: %s", description, msg);
    bus_send_message(msg);
    vTaskDelay(pdMS_TO_TICKS(REQUEST_INTERVAL_MS));
}

static void register_requester_task(void *arg)
{
    ESP_LOGI(TAG, "Task started, waiting %d ms for Internet Gateway...", STARTUP_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(STARTUP_DELAY_MS));

    while (1) {
        // Snapshot the relevant state under the mutex
        bool gateway_present = false;
        bool timers_valid[MAX_TIMERS] = {0};
        bool light_configured[MAX_LIGHT_ZONES] = {0};
        bool light_multicolor_valid[MAX_LIGHT_ZONES] = {0};
        bool light_name_valid[MAX_LIGHT_ZONES] = {0};
        bool valve_configured[MAX_VALVE_SLOTS] = {0};
        bool valve_label_valid[MAX_VALVE_SLOTS] = {0};

        if (s_pool_state_mutex &&
            xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            gateway_present = s_pool_state.gateway_ip_valid;
            for (int i = 0; i < MAX_TIMERS; i++) {
                timers_valid[i] = s_pool_state.timers[i].valid;
            }
            for (int i = 0; i < MAX_LIGHT_ZONES; i++) {
                light_configured[i]       = s_pool_state.lighting[i].configured;
                light_multicolor_valid[i] = s_pool_state.lighting[i].multicolor_valid;
                light_name_valid[i]       = s_pool_state.lighting[i].name_valid;
            }
            for (int i = 0; i < MAX_VALVE_SLOTS; i++) {
                valve_configured[i] = s_pool_state.valves[i].configured;
                uint8_t reg_id = 0xD0 + i;
                for (int j = 0; j < 32; j++) {
                    if (s_pool_state.register_labels[j].valid &&
                        s_pool_state.register_labels[j].reg_id == reg_id) {
                        valve_label_valid[i] = true;
                        break;
                    }
                }
            }
            xSemaphoreGive(s_pool_state_mutex);
        }

        if (gateway_present) {
            ESP_LOGD(TAG, "Internet Gateway present, no requests needed");
        } else {
            // Request missing timers
            int missing_timers = 0;
            for (int i = 0; i < MAX_TIMERS; i++) {
                if (!timers_valid[i]) missing_timers++;
            }
            if (missing_timers > 0) {
                ESP_LOGI(TAG, "Requesting %d missing timer(s)", missing_timers);
                for (int i = 0; i < MAX_TIMERS; i++) {
                    if (!timers_valid[i]) {
                        char desc[16];
                        snprintf(desc, sizeof(desc), "timer %d", i + 1);
                        send_request(0x08 + i, 0x04, desc);
                    }
                }
            }

            // Request missing light zone multicolor and name for configured zones
            for (int i = 0; i < MAX_LIGHT_ZONES; i++) {
                if (!light_configured[i]) continue;

                if (!light_multicolor_valid[i]) {
                    char desc[24];
                    snprintf(desc, sizeof(desc), "light %d multicolor", i + 1);
                    send_request(0xA0 + i, 0x01, desc);
                }
                if (!light_name_valid[i]) {
                    char desc[24];
                    snprintf(desc, sizeof(desc), "light %d name", i + 1);
                    send_request(0xB0 + i, 0x01, desc);
                }
            }

            // Request missing valve labels for configured valves
            for (int i = 0; i < MAX_VALVE_SLOTS; i++) {
                if (!valve_configured[i]) continue;
                if (!valve_label_valid[i]) {
                    char desc[24];
                    snprintf(desc, sizeof(desc), "valve %d label", i + 1);
                    send_request(0xD0 + i, 0x02, desc);
                }
            }
        }

        // Wait for retry interval or an early wake from register_requester_notify()
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_INTERVAL_MS));
    }
}

void register_requester_notify(void)
{
    if (s_task_handle) {
        xTaskNotifyGive(s_task_handle);
    }
}

void register_requester_start(void)
{
    xTaskCreate(register_requester_task, "reg_req", 4096, NULL, 2, &s_task_handle);
}
