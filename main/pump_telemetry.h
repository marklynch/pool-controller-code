#ifndef PUMP_TELEMETRY_H
#define PUMP_TELEMETRY_H

#include <stdbool.h>

/**
 * Persistence for "this install has a pump that reports its own speed and
 * power" (CMD 0x3B broadcasts, roughly once a minute). The live flag is
 * pool_state_t.pump_telemetry_seen; this module only keeps it across reboots.
 *
 * The broadcast is the only evidence, so without persistence a reboot would
 * forget it until the next one arrives, and turning the pump off in that
 * window would leave its last speed and power published indefinitely.
 */

// Whether a pump broadcast was ever seen, as stored. Call once at startup, to
// seed pool_state_t.pump_telemetry_seen before the decoder runs.
bool pump_telemetry_load_seen(void);

// Store that a pump broadcast has been seen. The caller calls this when the
// live flag first becomes true, so NVS is written once ever, not on every
// broadcast.
void pump_telemetry_save_seen(void);

#endif // PUMP_TELEMETRY_H
