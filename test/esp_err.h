/**
 * Mock esp_err.h for host-based testing
 *
 * This provides minimal ESP error type definitions for testing without ESP-IDF.
 */

#ifndef ESP_ERR_H
#define ESP_ERR_H

#include <stdint.h>

// ESP error type
typedef int32_t esp_err_t;

// Common error codes
#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  0x101
#define ESP_ERR_INVALID_ARG  0x102
#define ESP_ERR_INVALID_STATE  0x103
#define ESP_ERR_NOT_FOUND  0x105

// Error-name lookup. The real implementation returns a symbolic name; the
// stub only needs something printable and stable.
static inline const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK:   return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        default:       return "ESP_ERR";
    }
}

#endif // ESP_ERR_H
