#ifndef UNKNOWN_BUFFER_H
#define UNKNOWN_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "config.h"

#define UNKNOWN_BUFFER_CAPACITY         100
#define UNKNOWN_BUFFER_MAX_RAW_BYTES    64   // store at most the first 64 bytes of each frame

// Why a frame was captured. UNHANDLED (no handler matched) and
// UNDOCUMENTED_PAYLOAD (handler matched, but a field carried an undocumented
// value) are *not* errors — they are valid frames flagged for protocol
// research. The remaining reasons are framing/validation errors. The framing
// layer now records every resync type (including the per-byte BAD_* steps) so
// the offending bytes are visible on the Unknown Messages page.
typedef enum {
    UNKNOWN_REASON_UNHANDLED = 0,         // valid frame, no handler matched
    UNKNOWN_REASON_UNDOCUMENTED_PAYLOAD,  // handler matched, but a field value is undocumented
    UNKNOWN_REASON_NO_START,        // framing: no START byte in buffer
    UNKNOWN_REASON_BUFFER_OVERFLOW, // framing: buffer filled without a complete frame
    UNKNOWN_REASON_BAD_FRAMING,     // decoder: too short / missing START/END (guards test-decode)
    UNKNOWN_REASON_HEADER_CHECKSUM, // framing: header checksum (byte 9) mismatch
    UNKNOWN_REASON_DATA_CHECKSUM,   // framing: data checksum mismatch
    UNKNOWN_REASON_BAD_FRAME_TYPE,  // framing: frame type bytes (5-6) not 80 00 or 00 00
    UNKNOWN_REASON_BAD_LENGTH,      // framing: length field (byte 8) out of range
    UNKNOWN_REASON_BAD_END,         // framing: end byte != 0x03 at declared length
    UNKNOWN_REASON_UNEXPECTED,      // resync/result type not recognized by the mapper
} unknown_reason_t;

typedef struct {
    uint16_t raw_len;      // actual frame length (may exceed UNKNOWN_BUFFER_MAX_RAW_BYTES)
    uint8_t  raw[UNKNOWN_BUFFER_MAX_RAW_BYTES];  // first bytes of the frame
    uint32_t hit_count;
    time_t   first_seen;   // UTC epoch seconds (0 if NTP not synced)
    time_t   last_seen;    // UTC epoch seconds (0 if NTP not synced)
    unknown_reason_t reason;  // why this frame was captured
    bool     is_error;     // derived: true only for framing/validation reasons
} unknown_entry_t;

void unknown_buffer_init(void);

// Record a frame, tagged with why it was captured (see unknown_reason_t).
void unknown_buffer_record(const uint8_t *data, int len, unknown_reason_t reason);

// Short human-readable label for a reason (e.g. "no start", "header checksum").
const char *unknown_reason_str(unknown_reason_t reason);

void unknown_buffer_clear(void);

typedef struct {
    const unknown_entry_t *entries;
    int                   count;
} locked_unknown_buffer_t;

// Acquires the buffer mutex and returns a read-only pointer to the entry array.
// The caller MUST call unknown_buffer_unlock_after_read() when done.
locked_unknown_buffer_t unknown_buffer_lock_for_read(void);
void unknown_buffer_unlock_after_read(void);

#endif // UNKNOWN_BUFFER_H
