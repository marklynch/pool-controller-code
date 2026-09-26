#ifndef FILTER_PUMP_TYPE_H
#define FILTER_PUMP_TYPE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Whether the Filter channel drives a multi-speed pump.
 *
 * The bus never states this outright, so it is inferred from the states the
 * Touchscreen reports for the Filter channel in CMD 0x0B: a multi-speed
 * channel uses Low/Medium/High (0x03-0x05) where a single-speed channel uses
 * the plain On (0x02), and the two sets never overlap. Off and Auto are
 * common to both and carry no information.
 *
 * Until one of those states has been observed the answer is genuinely
 * unknown, so it is a tri-state rather than a bool — "not yet multi" and
 * "known single" lead to different MQTT option lists.
 *
 * Persisted in NVS, because the evidence only arrives while the pump is
 * running: a reboot with the Filter channel Off would otherwise throw the
 * answer away and wait for the next run to learn it again.
 */
typedef enum {
    FILTER_PUMP_TYPE_UNKNOWN      = 0,
    FILTER_PUMP_TYPE_SINGLE_SPEED = 1,
    FILTER_PUMP_TYPE_MULTI_SPEED  = 2,
} filter_pump_type_t;

// Load the stored type. Call once at startup, before MQTT comes up.
void filter_pump_type_init(void);

filter_pump_type_t filter_pump_type_get(void);

/**
 * Work out the filter pump's type from a state the Touchscreen reported for
 * the Filter channel (CMD 0x0B code space), and store it. Safe to call on
 * every broadcast: a state that settles nothing, or that agrees with what is
 * already known, touches neither RAM nor NVS.
 *
 * Returns true when the answer changed, meaning the MQTT option list is now
 * stale and discovery should be republished.
 */
bool filter_pump_type_learn(uint8_t channel_state);

#endif // FILTER_PUMP_TYPE_H
