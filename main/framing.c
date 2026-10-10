#include "framing.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BUS_FRAMING";

// Header validation only ever inspects bytes 0-9 (see layout in framing.h);
// cap early-failure captures to that window rather than the whole remaining
// buffer. Otherwise the captured bytes (and length) vary with how much
// trailing valid data happens to be queued, which both pollutes the
// unknown-message diagnostics with unrelated trailing frames and defeats
// dedup in unknown_buffer_record (which keys on reason + len + raw bytes).
#define FRAMING_HEADER_CAPTURE_LEN 10

// Format up to `cap` bytes of `data` as a space-separated hex string into
// `hex_str` (size `hex_str_size`). Does not append a truncation marker —
// callers that care whether `len > cap` add their own suffix to the log line.
static void framing_format_hex_dump(const uint8_t *data, int len, int cap, char *hex_str, size_t hex_str_size)
{
    int hex_pos = 0;
    int dump_len = MIN(len, cap);
    for (int i = 0; i < dump_len && hex_pos < (int)hex_str_size - 3; i++) {
        hex_pos += snprintf(&hex_str[hex_pos], hex_str_size - hex_pos, "%02X ", data[i]);
    }
    hex_str[hex_pos] = '\0';
}

// Capture `capture_len` bytes from the front of `fb` into out_frame/out_len
// for diagnostics, then resync by discarding a single byte and shifting the
// rest of the buffer down. The per-byte memmove is O(n), so draining a full
// garbage buffer one byte at a time is O(n^2) - acceptable only because
// BUS_MESSAGE_MAX_SIZE is small (256B).
static void framing_resync_one_byte(framing_buffer_t *fb, uint8_t *out_frame, int *out_len, int capture_len)
{
    if (out_frame) {
        memcpy(out_frame, fb->buffer, capture_len);
    }
    if (out_len) {
        *out_len = capture_len;
    }
    memmove(fb->buffer, &fb->buffer[1], fb->len - 1);
    fb->len--;
}

framing_packet_type_t framing_classify_packet(uint8_t frame_type_hi, uint8_t frame_type_lo)
{
    if (frame_type_hi == 0x80 && frame_type_lo == 0x00) {
        return FRAME_WITH_DATA;
    }
    if (frame_type_hi == 0x00 && frame_type_lo == 0x00) {
        return FRAME_NO_DATA;
    }
    return FRAME_INVALID;
}

void framing_init(framing_buffer_t *fb)
{
    if (fb) {
        fb->len = 0;
    }
}

bool framing_add_bytes(framing_buffer_t *fb, const uint8_t *data, int len)
{
    if (!fb || !data || len <= 0) {
        return false;
    }
    if (fb->len + len > BUS_MESSAGE_MAX_SIZE) {
        return false;
    }
    memcpy(&fb->buffer[fb->len], data, len);
    fb->len += len;
    return true;
}

framing_result_t framing_process_next(framing_buffer_t *fb, uint8_t *out_frame, int *out_len)
{
    if (!fb) {
        return FRAMING_NEED_MORE_DATA;
    }

    // Need at least minimum message: START + SRC + DST + FRAME_TYPE + CMD + LEN + HCHK + END = 11 bytes
    // (the smallest valid frame has zero payload bytes and no separate data
    // checksum byte - see the msg_len == 11 special case below)
    if (fb->len < 11) {
        return FRAMING_NEED_MORE_DATA;
    }

    // Find start byte (0x02)
    int start_idx = -1;
    for (int i = 0; i < fb->len; i++) {
        if (fb->buffer[i] == 0x02) {
            start_idx = i;
            break;
        }
    }

    // No start byte found - discard everything
    if (start_idx == -1) {
        char hex_str[100];
        framing_format_hex_dump(fb->buffer, fb->len, 32, hex_str, sizeof(hex_str));
        ESP_LOGW(TAG, "No start byte in buffer, discarding %d bytes: %s%s",
                 fb->len, hex_str, fb->len > 32 ? "..." : "");
        // Hand the discarded bytes back so the caller can capture them.
        if (out_frame) {
            memcpy(out_frame, fb->buffer, fb->len);
        }
        if (out_len) {
            *out_len = fb->len;
        }
        fb->len = 0;
        return FRAMING_NO_START_BYTE;
    }

    // Discard bytes before start. This happens silently with respect to the
    // resync counters/unknown buffer (those only track the byte-stepped BAD_*
    // resyncs and the no-start-at-all case) - log it so a multi-byte jump like
    // this is still visible somewhere.
    if (start_idx > 0) {
        char hex_str[100];
        framing_format_hex_dump(fb->buffer, start_idx, 32, hex_str, sizeof(hex_str));
        ESP_LOGW(TAG, "Discarding %d byte(s) before next start byte: %s%s",
                 start_idx, hex_str, start_idx > 32 ? "..." : "");
        memmove(fb->buffer, &fb->buffer[start_idx], fb->len - start_idx);
        fb->len -= start_idx;
    }

    // Check we have enough for header validation (10 bytes: 0..9)
    if (fb->len < 10) {
        return FRAMING_NEED_MORE_DATA;
    }

    // Validate header checksum: byte 9 must be sum(bytes 0-8) & 0xFF
    // We test checksum first, and only then the other header bytes - this way,
    // a bad length will only survive if the checksum is coincidentally correct.
    uint8_t expected_hchk = 0;
    for (int i = 0; i < 9; i++) {
        expected_hchk += fb->buffer[i];
    }
    if (fb->buffer[9] != expected_hchk) {
        char hex_str[100];
        framing_format_hex_dump(fb->buffer, fb->len, 32, hex_str, sizeof(hex_str));
        ESP_LOGW(TAG, "Invalid header checksum: calculated %02X, got %02X, data: %s%s, resyncing by 1 byte",
                 expected_hchk, fb->buffer[9], hex_str, fb->len > 32 ? "..." : "");
        framing_resync_one_byte(fb, out_frame, out_len, MIN(fb->len, FRAMING_HEADER_CAPTURE_LEN));
        return FRAMING_BAD_HEADER_CHECKSUM;
    }

    // Validate frame type bytes (positions 5-6) and classify the packet type
    framing_packet_type_t packet_type = framing_classify_packet(fb->buffer[5], fb->buffer[6]);
    if (packet_type == FRAME_INVALID) {
        ESP_LOGW(TAG, "Invalid frame type bytes: %02X %02X (expected 80 00 or 00 00), resyncing by 1 byte",
                 fb->buffer[5], fb->buffer[6]);
        framing_resync_one_byte(fb, out_frame, out_len, MIN(fb->len, FRAMING_HEADER_CAPTURE_LEN));
        return FRAMING_BAD_FRAME_TYPE;
    }

    // Read length from byte 8. Frames with no data are always exactly 11
    // bytes (header + END, no payload, no data checksum); frames with data
    // are always at least 12 (header + data-checksum byte + END, plus any
    // payload).
    int msg_len = fb->buffer[8];
    if (packet_type == FRAME_NO_DATA) {
        if (msg_len != 11) {
            ESP_LOGW(TAG, "Invalid length field for frame with no data: 0x%02X (%d), expected 0x0B (11), resyncing by 1 byte",
                     msg_len, msg_len);
            framing_resync_one_byte(fb, out_frame, out_len, MIN(fb->len, FRAMING_HEADER_CAPTURE_LEN));
            return FRAMING_BAD_LENGTH;
        }
    } else {
        if (msg_len < 12 || msg_len > BUS_MESSAGE_MAX_SIZE) {
            ESP_LOGW(TAG, "Invalid length field for frame with data: 0x%02X (%d), expected >= 0x0C (12), resyncing by 1 byte",
                     msg_len, msg_len);
            framing_resync_one_byte(fb, out_frame, out_len, MIN(fb->len, FRAMING_HEADER_CAPTURE_LEN));
            return FRAMING_BAD_LENGTH;
        }
    }

    // Check if we have received the full message yet
    if (fb->len < msg_len) {
        return FRAMING_NEED_MORE_DATA;
    }

    // Verify end byte (0x03)
    if (fb->buffer[msg_len - 1] != 0x03) {
        ESP_LOGW(TAG, "Invalid end byte (expected 03, got %02X) at length %d, resyncing by 1 byte",
                 fb->buffer[msg_len - 1], msg_len);
        framing_resync_one_byte(fb, out_frame, out_len, msg_len);
        return FRAMING_BAD_END_BYTE;
    }

    // Verify data checksum: sum(bytes 10..msg_len-3) & 0xFF.
    // Frames with no data have no data-checksum byte at all (see above), so
    // there's nothing to verify for them.
    if (packet_type == FRAME_WITH_DATA) {
        uint32_t data_sum = 0;
        for (int i = 10; i < msg_len - 2; i++) {
            data_sum += fb->buffer[i];
        }
        uint8_t calculated_dchk = data_sum & 0xFF;
        if (fb->buffer[msg_len - 2] != calculated_dchk) {
            ESP_LOGW(TAG, "Invalid data checksum: calculated %02X, got %02X, resyncing by 1 byte",
                     calculated_dchk, fb->buffer[msg_len - 2]);
            framing_resync_one_byte(fb, out_frame, out_len, msg_len);
            return FRAMING_BAD_DATA_CHECKSUM;
        }
    }

    // Valid frame — copy out before consuming from buffer
    if (out_frame) {
        memcpy(out_frame, fb->buffer, msg_len);
    }
    if (out_len) {
        *out_len = msg_len;
    }

    // Remove processed message from buffer
    int remaining = fb->len - msg_len;
    if (remaining > 0) {
        memmove(fb->buffer, &fb->buffer[msg_len], remaining);
    }
    fb->len = remaining;

    return FRAMING_FRAME_READY;
}
