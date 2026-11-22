#ifndef REGISTER_REQUESTER_H
#define REGISTER_REQUESTER_H

#include "pool_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Start the register requester task.
// If no Internet Gateway is detected after a startup delay, this task will
// send CMD 0x39 register read requests for any missing pool state data, and
// periodically re-read the timers and favourites, which the Touchscreen never
// re-sends, even after an edit made at the Touchscreen.
void register_requester_start(pool_state_t *pool_state, SemaphoreHandle_t state_mutex);

// Wake the requester task immediately to check for missing data.
// Safe to call from any task context (e.g. when a light zone is first configured).
void register_requester_notify(void);

// Queue a one-off CMD 0x39 read of a register, sent after a short delay.
// Used to read a register back after writing it: the controller does not
// announce every change itself, and the Internet Gateway's polling cycle is
// install-dependent, so without this a write can never reach Home Assistant.
// Runs regardless of whether a Gateway is present.
// Safe to call from any task context.
void register_requester_read_back(uint8_t reg_id, uint8_t slot);

#endif // REGISTER_REQUESTER_H
