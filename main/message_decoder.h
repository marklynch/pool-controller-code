#ifndef MESSAGE_DECODER_H
#define MESSAGE_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include "pool_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Message decoder configuration
 *
 * This structure holds the context needed for decoding pool bus messages.
 * For unit testing, enable_mqtt can be set to false to skip MQTT publishing.
 */
typedef struct {
    pool_state_t *pool_state;          // Pointer to global pool state
    SemaphoreHandle_t state_mutex;     // Mutex protecting pool state
    bool enable_mqtt;                   // If false, skip MQTT publishing (for testing)
} message_decoder_context_t;

/**
 * Decode a pool bus message
 *
 * Parses and processes messages from the Astral pool controller bus.
 * Updates pool state and optionally publishes to MQTT.
 *
 * @param data Message bytes (must start with 0x02, end with 0x03)
 * @param len Length of message in bytes
 * @param ctx Decoder context (pool state, mutex, MQTT enable flag)
 * @return true if message was decoded, false if unknown/malformed
 */
bool decode_message(const uint8_t *data, int len, message_decoder_context_t *ctx);

/**
 * Verify checksum for Astral protocol messages
 *
 * Checksum = sum of all data bytes from index 10 to (len-3), low byte only.
 * The checksum byte is at (len-2).
 *
 * @param data Message bytes
 * @param len Length of message
 * @return true if checksum is valid, false otherwise
 */
bool verify_message_checksum(const uint8_t *data, int len);

/**
 * Get device name from address bytes
 *
 * @param addr_hi High byte of address
 * @param addr_lo Low byte of address
 * @return Device name string, or NULL if unknown
 */
const char* get_device_name(uint8_t addr_hi, uint8_t addr_lo);

/**
 * Get gateway communications status text
 *
 * @param code Status code (big-endian value from message)
 * @return Status text string
 */
const char* get_gateway_comms_status_text(uint16_t code);

// External constant arrays (defined in message_decoder.c)
extern const char *CHANNEL_STATE_NAMES[];
extern const char *LIGHTING_STATE_NAMES[];
extern const char *LIGHTING_COLOR_NAMES[];

// Constant counts
#define CHANNEL_STATE_COUNT 6
#define LIGHTING_STATE_COUNT 3
#define LIGHTING_COLOR_COUNT 51

/**
 * Get channel type name from type code
 * Supports all channel types including special codes (0xFD=Heater, 0xFE=Light Zone)
 * @param type_code Channel type code
 * @return Channel type name, or "Unknown" if not found
 */
const char* get_channel_type_name(uint8_t type_code);

// Special channel markers
#define CHANNEL_UNUSED     0x00  // Unused/unconfigured channel

#endif // MESSAGE_DECODER_H
