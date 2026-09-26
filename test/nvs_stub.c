/**
 * In-memory NVS stub for host-based testing.
 *
 * Just enough to let modules that persist a setting run on the host: a flat
 * key/value table keyed on namespace + key, holding the value sizes the
 * firmware actually stores. Values survive nvs_close, so a test can reset a
 * module's RAM cache, call its init(), and check that the stored value comes
 * back — the reboot case that motivates persisting at all.
 */

#include "nvs.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_ENTRIES 16
#define MAX_NAME    32

typedef struct {
    char ns[MAX_NAME];
    char key[MAX_NAME];
    uint16_t value;
    bool in_use;
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static char s_handle_ns[8][MAX_NAME];
static int s_next_handle = 1;

void nvs_stub_reset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_next_handle = 1;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (s_next_handle >= (int)(sizeof(s_handle_ns) / sizeof(s_handle_ns[0]))) return ESP_FAIL;
    int h = s_next_handle++;
    snprintf(s_handle_ns[h], MAX_NAME, "%s", namespace_name);
    *out_handle = h;
    return ESP_OK;
}

static entry_t *find(nvs_handle_t handle, const char *key, bool create)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].in_use &&
            strcmp(s_entries[i].ns, s_handle_ns[handle]) == 0 &&
            strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    if (!create) return NULL;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!s_entries[i].in_use) {
            s_entries[i].in_use = true;
            snprintf(s_entries[i].ns, MAX_NAME, "%s", s_handle_ns[handle]);
            snprintf(s_entries[i].key, MAX_NAME, "%s", key);
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    entry_t *e = find(handle, key, false);
    if (!e) return ESP_FAIL;
    *out_value = (uint8_t)e->value;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    entry_t *e = find(handle, key, true);
    if (!e) return ESP_FAIL;
    e->value = value;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value)
{
    entry_t *e = find(handle, key, false);
    if (!e) return ESP_FAIL;
    *out_value = e->value;
    return ESP_OK;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    entry_t *e = find(handle, key, true);
    if (!e) return ESP_FAIL;
    e->value = value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
