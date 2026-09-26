/**
 * Channel state name table
 *
 * The state codes broadcast per channel by CMD 0x0B, and accepted as the
 * target state by CMD 0x0F — see PROTOCOL.md, 0x0B Channel Status.
 *
 * These strings are also the pump-mode select's MQTT option list, published
 * by mqtt_discovery.c and parsed back by mqtt_commands.c, so a second copy of
 * them would be a silent drift risk.
 *
 * Kept in its own translation unit, so the host-side unit tests can link the
 * real table without pulling in all of message_decoder.c.
 */

#include "pool_state.h"

const char *CHANNEL_STATE_NAMES[] = {
    "Off",      // 0
    "Auto",     // 1
    "On",       // 2
    "Low",      // 3
    "Medium",   // 4
    "High",     // 5
};
