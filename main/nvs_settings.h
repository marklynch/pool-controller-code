#ifndef NVS_SETTINGS_H
#define NVS_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/**
 * One-value NVS settings: open the namespace, read or write a single u8, close.
 * For modules that keep a flag or small code in their own namespace.
 */

// Read `key` from namespace `ns` into *out. Returns false (leaving *out
// untouched) if nothing is stored yet or NVS can't be read, so the caller
// keeps its default.
bool nvs_settings_get_u8(const char *ns, const char *key, uint8_t *out);

// Store and commit `value` under `key` in namespace `ns`. Failures are logged,
// not returned: the caller's in-memory value still applies for this boot, only
// persistence is lost.
void nvs_settings_set_u8(const char *ns, const char *key, uint8_t value);

#endif // NVS_SETTINGS_H
