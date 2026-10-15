#include "heater_freshness_logic.h"
#include "config.h"
#include <stddef.h>

uint32_t heater_freshness_expire(pool_state_t *state, uint32_t now_ms,
                                 uint32_t timeout_ms)
{
    if (state == NULL) return 0;

    uint32_t expired_mask = 0;
    for (int heater_idx = 0; heater_idx < MAX_HEATERS; heater_idx++) {
        pool_heater_t *heater = &state->heaters[heater_idx];
        if (!heater->communications_online ||
            (uint32_t)(now_ms - heater->device_last_response_ms) < timeout_ms) {
            continue;
        }

        heater->communications_online = false;
        heater->gas_heater_valid = false;
        expired_mask |= 1U << heater_idx;

        for (int dev_idx = 0; dev_idx < state->num_seen_devices; dev_idx++) {
            seen_device_t *device = &state->seen_devices[dev_idx];
            uint16_t address = ((uint16_t)device->addr_hi << 8) | device->addr_lo;
            if (address == heater->device_address) {
                device->temp1_valid = false;
                device->temp2_valid = false;
            }
        }
    }

    return expired_mask;
}
