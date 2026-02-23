#include "device_serial.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "DEVICE_SERIAL";

// Crockford Base32 alphabet — excludes I, L, O, U to reduce transcription errors
static const char CROCKFORD32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static char s_serial[DEVICE_SERIAL_LEN] = {0};

/**
 * Encode `data_len` bytes from `data` into Crockford Base32, writing the
 * null-terminated result into `out` (at most `out_len` bytes including null).
 *
 * Bits are consumed MSB-first.  Any trailing bits shorter than 5 are
 * zero-padded on the right before encoding.
 */
static void crockford32_encode(const uint8_t *data, size_t data_len,
                               char *out, size_t out_len)
{
    uint64_t bits = 0;
    int bit_count = 0;
    size_t out_pos = 0;

    for (size_t i = 0; i < data_len && out_pos < out_len - 1; i++) {
        bits = (bits << 8) | data[i];
        bit_count += 8;
        while (bit_count >= 5 && out_pos < out_len - 1) {
            bit_count -= 5;
            out[out_pos++] = CROCKFORD32[(bits >> bit_count) & 0x1F];
        }
    }

    // Flush any remaining bits (zero-pad on the right to complete 5-bit group)
    if (bit_count > 0 && out_pos < out_len - 1) {
        out[out_pos++] = CROCKFORD32[(bits << (5 - bit_count)) & 0x1F];
    }

    out[out_pos] = '\0';
}

void device_get_serial(char *buf, size_t len)
{
    if (s_serial[0] != '\0') {
        strncpy(buf, s_serial, len - 1);
        buf[len - 1] = '\0';
        return;
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read eFuse MAC address");
        strncpy(buf, "0000000000", len - 1);
        buf[len - 1] = '\0';
        return;
    }

    crockford32_encode(mac, sizeof(mac), s_serial, sizeof(s_serial));
    ESP_LOGI(TAG, "Device serial: %s  (MAC %02X:%02X:%02X:%02X:%02X:%02X)",
             s_serial,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    strncpy(buf, s_serial, len - 1);
    buf[len - 1] = '\0';
}
