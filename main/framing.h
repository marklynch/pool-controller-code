#ifndef FRAMING_H
#define FRAMING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

typedef enum {
    FRAMING_NEED_MORE_DATA,        // incomplete, wait for more bytes
    FRAMING_NO_START_BYTE,         // no 0x02 found, buffer cleared
    FRAMING_BAD_HEADER_CHECKSUM,   // header checksum mismatch, resynced by 1
    FRAMING_BAD_FRAME_TYPE,        // bytes 5-6 not 0x80 0x00 or 0x00 0x00, resynced by 1
    FRAMING_BAD_LENGTH,            // length field out of range, resynced by 1
    FRAMING_BAD_END_BYTE,          // end byte not 0x03, resynced by 1
    FRAMING_BAD_DATA_CHECKSUM,     // data checksum mismatch, resynced by 1
    FRAMING_FRAME_READY,           // valid frame written to out_frame
} framing_result_t;

// The frame type bytes (offsets 5-6) determine the shape of the rest of the
// frame. A frame with data (0x80 0x00) always carries a data-checksum byte
// (offset N-2), even with zero payload bytes, so its minimum length is 12.
// A frame with no data (0x00 0x00) carries no payload and no separate
// data-checksum byte at all - it's exactly 11 bytes (header + END), and
// byte 9 is purely the header checksum. Shared between the reassembly layer
// (framing.c) and the decoder (message_decoder.c), which both need to branch
// on this classification rather than re-deriving it from raw frame type bytes.
typedef enum {
    FRAME_WITH_DATA,  // frame type bytes 0x80 0x00
    FRAME_NO_DATA,    // frame type bytes 0x00 0x00
    FRAME_INVALID,    // anything else
} framing_packet_type_t;

/**
 * Classify a frame's type bytes (offsets 5-6) into a packet type.
 */
framing_packet_type_t framing_classify_packet(uint8_t frame_type_hi, uint8_t frame_type_lo);

// Frames carrying a secret: the Touchscreen writing the Internet Gateway's
// WiFi password (CMD 0x3A register write to register 0xD1, slot 0x07). The
// value bytes after register and slot are masked wherever a frame is logged
// as hex, so the password never reaches the console, port 7373 or the web
// Debug Log (whose downloads are meant for public bug reports).
#define FRAMING_CMD_REGISTER_WRITE     0x3A
#define FRAMING_SECRET_REG_ID          0xD1
#define FRAMING_SECRET_SLOT            0x07
#define FRAMING_PAYLOAD_OFFSET         10    // First payload byte; the register write's register ID
#define FRAMING_SECRET_VALUE_OFFSET    12    // First value byte, after register ID and slot

/**
 * Format a frame as space-separated hex ("02 00 50 ... 03 ") for logging,
 * writing secret value bytes (see FRAMING_SECRET_*) as "**". Writes whole
 * bytes only and always NUL-terminates; returns the string length.
 */
int framing_format_hex(const uint8_t *frame, int len, char *out, size_t out_size);

// Context structure holding the reassembly buffer
typedef struct {
    uint8_t buffer[BUS_MESSAGE_MAX_SIZE];
    int len;
} framing_buffer_t;

/**
 * Initialize a framing buffer context.
 */
void framing_init(framing_buffer_t *fb);

/**
 * Add bytes to the framing buffer.
 * Returns true if bytes were successfully added, false if buffer is full.
 */
bool framing_add_bytes(framing_buffer_t *fb, const uint8_t *data, int len);

/**
 * Extract one complete message from the reassembly buffer.
 * On FRAMING_FRAME_READY the frame is written to out_frame and out_len; on
 * every other resync result (NO_START / BAD_*) the bytes that triggered the
 * resync are written there instead, so the caller can capture them.
 * FRAMING_NEED_MORE_DATA means stop and wait; all other results mean call
 * again immediately.
 */
framing_result_t framing_process_next(framing_buffer_t *fb, uint8_t *out_frame, int *out_len);

#endif // FRAMING_H
