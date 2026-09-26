/**
 * Mock nvs.h for host-based testing.
 *
 * Backed by a tiny in-memory store (nvs_stub.c) so modules that persist a
 * setting can be exercised on the host, including across a simulated reboot.
 */

#ifndef NVS_H
#define NVS_H

#include <stdint.h>
#include "esp_err.h"

typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef int nvs_handle_t;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value);
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

// Test-only: forget everything, as if the flash had been erased.
void nvs_stub_reset(void);

#endif // NVS_H
