#include "register_requester.h"
#include "message_decoder.h"
#include "bus.h"
#include "pool_state.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "REG_REQ";
static pool_state_t *s_state = NULL;
static SemaphoreHandle_t s_mutex = NULL;

// Wait this long at startup before checking — gives the gateway time to announce itself
#define STARTUP_DELAY_MS     55000
// Delay between individual requests in a sequence
#define REQUEST_INTERVAL_MS  250
// How long to wait before re-checking if data is still missing
#define RETRY_INTERVAL_MS    60000
// How long to let the controller apply a write before reading the register back
#define READBACK_DELAY_MS    1000
// Pending read-backs / wake-ups waiting to be serviced by the task
#define REQUEST_QUEUE_LEN    8
// How many sweeps to ask for a heater setpoint before giving up on it. Unlike
// a light zone or valve, nothing in pool_state says whether a heater slot
// exists, so an unconfigured slot is indistinguishable from a configured one
// that has not answered yet — bound the retries instead.
#define HEATER_SETPOINT_MAX_ATTEMPTS 5

// A unit of work posted to the task. `rescan` requests a fresh check for
// missing data; otherwise the entry names a register to read back.
typedef struct {
    uint8_t reg_id;
    uint8_t slot;
    bool    rescan;
} reg_request_t;

static QueueHandle_t s_queue = NULL;

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

// The setpoint registers to sweep for, one entry per heater circuit (all slot
// 0x00): Heater 1 is 0xE7/0xE8, Heater 2 is 0xEA/0xEB — not a contiguous
// stride, so spell them out.
static const struct {
    uint8_t reg_id;
    uint8_t heater_idx;
    bool    is_pool;
    const char *desc;
} HEATER_SETPOINT_REGS[] = {
    {REG_ID_HEATER1_POOL_SETPOINT, 0, true,  "heater 1 pool setpoint"},
    {REG_ID_HEATER1_SPA_SETPOINT,  0, false, "heater 1 spa setpoint"},
    {REG_ID_HEATER2_POOL_SETPOINT, 1, true,  "heater 2 pool setpoint"},
    {REG_ID_HEATER2_SPA_SETPOINT,  1, false, "heater 2 spa setpoint"},
};
#define HEATER_SETPOINT_REG_COUNT \
    (sizeof(HEATER_SETPOINT_REGS) / sizeof(HEATER_SETPOINT_REGS[0]))
_Static_assert(MAX_HEATERS == 2,
               "HEATER_SETPOINT_REGS needs an entry per heater circuit");

// Sweeps spent asking for each setpoint register, indexed as
// HEATER_SETPOINT_REGS. Task-local state: only ever touched from
// register_requester_task().
static uint8_t s_heater_setpoint_attempts[HEATER_SETPOINT_REG_COUNT];

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
        bool fav_enabled_valid[MAX_FAVOURITES] = {0};
        bool fav_name_valid[MAX_FAVOURITES] = {0};
        bool heater_pool_setpoint_valid[MAX_HEATERS] = {0};
        bool heater_spa_setpoint_valid[MAX_HEATERS] = {0};

        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            gateway_present = s_state->gateway_ip_valid;
            for (int i = 0; i < MAX_TIMERS; i++) {
                timers_valid[i] = s_state->timers[i].valid;
            }
            for (int i = 0; i < MAX_LIGHT_ZONES; i++) {
                light_configured[i]       = s_state->lighting[i].configured;
                light_multicolor_valid[i] = s_state->lighting[i].multicolor_valid;
                light_name_valid[i]       = s_state->lighting[i].name_valid;
            }
            for (int i = 0; i < MAX_VALVE_SLOTS; i++) {
                valve_configured[i] = s_state->valves[i].configured;
                uint8_t reg_id = REG_ID_VALVE_LABEL_0 + i;
                for (int j = 0; j < MAX_REGISTER_LABELS; j++) {
                    if (s_state->register_labels[j].valid &&
                        s_state->register_labels[j].reg_id == reg_id) {
                        valve_label_valid[i] = true;
                        break;
                    }
                }
            }
            for (int i = 0; i < MAX_FAVOURITES; i++) {
                fav_enabled_valid[i] = s_state->favourites[i].enabled_valid;
                fav_name_valid[i]    = s_state->favourites[i].name_valid;
            }
            for (int i = 0; i < MAX_HEATERS; i++) {
                heater_pool_setpoint_valid[i] = s_state->heaters[i].pool_setpoint_valid;
                heater_spa_setpoint_valid[i]  = s_state->heaters[i].spa_setpoint_valid;
            }
            xSemaphoreGive(s_mutex);
        }

        if (gateway_present) {
            ESP_LOGI(TAG, "Internet Gateway present, no requests needed");
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
                        send_request(REG_ID_TIMER_0 + i, 0x04, desc);
                    }
                }
            }

            // Request missing light zone multicolor and name for configured zones
            for (int i = 0; i < MAX_LIGHT_ZONES; i++) {
                if (!light_configured[i]) continue;

                if (!light_multicolor_valid[i]) {
                    ESP_LOGI(TAG, "Requesting missing light %d zone multicolor", i + 1);
                    char desc[24];
                    snprintf(desc, sizeof(desc), "light %d multicolor", i + 1);
                    send_request(REG_ID_LIGHT_ZONE_MULTICOLOR_0 + i, 0x01, desc);
                }
                if (!light_name_valid[i]) {
                    ESP_LOGI(TAG, "Requesting missing light %d zone name", i + 1);
                    char desc[24];
                    snprintf(desc, sizeof(desc), "light %d name", i + 1);
                    send_request(REG_ID_LIGHT_ZONE_NAME_0 + i, 0x01, desc);
                }
            }

            // Request missing valve labels for configured valves
            for (int i = 0; i < MAX_VALVE_SLOTS; i++) {
                if (!valve_configured[i]) continue;
                if (!valve_label_valid[i]) {
                    ESP_LOGI(TAG, "Requesting missing valve %d label", i + 1);
                    char desc[24];
                    snprintf(desc, sizeof(desc), "valve %d label", i + 1);
                    send_request(REG_ID_VALVE_LABEL_0 + i, 0x02, desc);
                }
            }

            // Request missing favourite enable flags and labels (all 8 slots)
            for (int i = 0; i < MAX_FAVOURITES; i++) {
                if (!fav_enabled_valid[i]) {
                    char desc[32];
                    snprintf(desc, sizeof(desc), "favourite %d enable", i);
                    send_request(REG_ID_FAVOURITE_ENABLE_0 + i, 0x03, desc);
                }
                if (!fav_name_valid[i]) {
                    char desc[32];
                    snprintf(desc, sizeof(desc), "favourite %d label", i);
                    send_request(REG_ID_FAVOURITE_LABEL_0 + i, 0x03, desc);
                }
            }

            // Request missing heater setpoints. On an install with no Gateway
            // whose touchscreen does not broadcast CMD 0x17, nothing else ever
            // populates them — the only other request is the read-back after an
            // MQTT setpoint write, so the value stays absent until the user
            // changes it from Home Assistant. The CMD 0x38 response carries the
            // heater slot in its register ID, so whichever device answers
            // (touchscreen or the heater itself) lands in the right slot.
            for (size_t i = 0; i < HEATER_SETPOINT_REG_COUNT; i++) {
                int idx = HEATER_SETPOINT_REGS[i].heater_idx;
                bool have = HEATER_SETPOINT_REGS[i].is_pool
                                ? heater_pool_setpoint_valid[idx]
                                : heater_spa_setpoint_valid[idx];
                if (have) continue;
                if (s_heater_setpoint_attempts[i] >= HEATER_SETPOINT_MAX_ATTEMPTS) continue;
                s_heater_setpoint_attempts[i]++;

                ESP_LOGI(TAG, "Requesting missing %s (attempt %d/%d)",
                         HEATER_SETPOINT_REGS[i].desc,
                         s_heater_setpoint_attempts[i], HEATER_SETPOINT_MAX_ATTEMPTS);
                send_request(HEATER_SETPOINT_REGS[i].reg_id, 0x00,
                             HEATER_SETPOINT_REGS[i].desc);
            }
        }

        // Serve queued read-backs until the retry interval elapses, then loop
        // round and re-check for missing data. A rescan request ends the wait
        // early. Read-backs are served whether or not a gateway is present.
        TickType_t remaining = pdMS_TO_TICKS(RETRY_INTERVAL_MS);
        while (remaining > 0) {
            reg_request_t req;
            TickType_t wait_start = xTaskGetTickCount();
            if (xQueueReceive(s_queue, &req, remaining) != pdTRUE) {
                break;  // retry interval elapsed
            }
            TickType_t waited = xTaskGetTickCount() - wait_start;
            remaining = (waited >= remaining) ? 0 : remaining - waited;

            if (req.rescan) {
                break;
            }

            // Give the controller time to apply the write it is following.
            vTaskDelay(pdMS_TO_TICKS(READBACK_DELAY_MS));
            char desc[32];
            snprintf(desc, sizeof(desc), "read-back of 0x%02X", req.reg_id);
            send_request(req.reg_id, req.slot, desc);
        }
    }
}

void register_requester_notify(void)
{
    if (!s_queue) return;

    reg_request_t req = { .rescan = true };
    xQueueSend(s_queue, &req, 0);
}

void register_requester_read_back(uint8_t reg_id, uint8_t slot)
{
    if (!s_queue) return;

    reg_request_t req = { .reg_id = reg_id, .slot = slot, .rescan = false };
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Read-back queue full, dropping 0x%02X/0x%02X", reg_id, slot);
    }
}

void register_requester_start(pool_state_t *pool_state, SemaphoreHandle_t state_mutex)
{
    s_state = pool_state;
    s_mutex = state_mutex;
    s_queue = xQueueCreate(REQUEST_QUEUE_LEN, sizeof(reg_request_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create request queue");
        return;
    }
    xTaskCreate(register_requester_task, "reg_req", 4096, NULL, 2, NULL);
}
