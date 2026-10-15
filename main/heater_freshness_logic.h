#ifndef HEATER_FRESHNESS_LOGIC_H
#define HEATER_FRESHNESS_LOGIC_H

#include "pool_state.h"
#include <stdint.h>

// Expire direct-heater telemetry older than timeout_ms. Returns a bit mask of
// heater indices whose communications state changed to offline.
uint32_t heater_freshness_expire(pool_state_t *state, uint32_t now_ms,
                                 uint32_t timeout_ms);

#endif // HEATER_FRESHNESS_LOGIC_H
