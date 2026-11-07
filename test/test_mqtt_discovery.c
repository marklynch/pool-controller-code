/*
 * Home Assistant discovery payloads, built by the real mqtt_discovery.c and
 * captured at mqtt_publish(). The linker strips the publishers these tests
 * don't call, along with whatever those need (see run_tests.sh).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "../main/device_serial.h"
#include "../main/mqtt_discovery.h"
#include "../main/mqtt_poolclient.h"
#include "../main/wifi_provisioning.h"
#include "esp_app_desc.h"

/* A cJSON just big enough to print what discovery builds: each node holds its
 * rendered JSON, and adding a child renders it into the parent and frees it. */
struct cJSON {
    char open, close;
    bool has_items;
    char text[2048];
};

static cJSON *new_node(char open, char close)
{
    cJSON *node = calloc(1, sizeof(*node));
    assert(node);
    node->open = open;
    node->close = close;
    return node;
}

static void append_item(cJSON *parent, const char *key, const char *value)
{
    size_t used = strlen(parent->text);
    int written = snprintf(parent->text + used, sizeof(parent->text) - used, "%s%s%s%s%s",
                           parent->has_items ? "," : "",
                           key ? "\"" : "", key ? key : "", key ? "\":" : "", value);
    assert(written > 0 && (size_t)written < sizeof(parent->text) - used);
    parent->has_items = true;
}

char *cJSON_PrintUnformatted(const cJSON *node)
{
    char *out = malloc(strlen(node->text) + 3);
    assert(out);
    sprintf(out, "%c%s%c", node->open, node->text, node->close);
    return out;
}

static void append_child(cJSON *parent, const char *key, cJSON *child)
{
    char *rendered = cJSON_PrintUnformatted(child);
    append_item(parent, key, rendered);
    free(rendered);
    free(child);
}

cJSON *cJSON_CreateObject(void) { return new_node('{', '}'); }
cJSON *cJSON_CreateArray(void) { return new_node('[', ']'); }
cJSON *cJSON_CreateString(const char *string)
{
    cJSON *node = new_node('"', '"');
    snprintf(node->text, sizeof(node->text), "%s", string);
    return node;
}
int cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    append_child(array, NULL, item);
    return 1;
}
int cJSON_AddItemToObject(cJSON *object, const char *key, cJSON *item)
{
    append_child(object, key, item);
    return 1;
}
cJSON *cJSON_AddStringToObject(cJSON *object, const char *key, const char *string)
{
    cJSON_AddItemToObject(object, key, cJSON_CreateString(string));
    return object;
}
void cJSON_Delete(cJSON *node) { free(node); }
void cJSON_free(void *ptr) { free(ptr); }

void mqtt_get_device_id(char *out, size_t size) { snprintf(out, size, "test"); }
void device_get_serial(char *out, size_t size) { snprintf(out, size, "SERIAL"); }
void device_get_mac_suffix(char *out, size_t size) { snprintf(out, size, "A1B2C3"); }
void device_get_mac_string(char *out, size_t size) { snprintf(out, size, "aa:bb:cc:a1:b2:c3"); }
const char *wifi_get_mdns_hostname(void) { return "pool-a1b2c3"; }
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = { .version = "test" };
    return &app;
}

static unsigned publish_count;
static char last_topic[256], last_payload[2048];
esp_err_t mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    assert(qos == 1 && retain);
    snprintf(last_topic, sizeof(last_topic), "%s", topic);
    int written = snprintf(last_payload, sizeof(last_payload), "%s", payload);
    assert(written > 0 && (size_t)written < sizeof(last_payload));
    publish_count++;
    return ESP_OK;
}

static void set_favourite(pool_state_t *state, int index, const char *name, bool enabled)
{
    favourite_t *favourite = &state->favourites[index];
    favourite->enabled = enabled;
    favourite->enabled_valid = true;
    if (name) {
        snprintf(favourite->name, sizeof(favourite->name), "%s", name);
        favourite->name_valid = true;
    }
}

/* The Touchscreen's favourites from the install that reported #156. */
static void set_reported_favourites(pool_state_t *state, bool pool_spa_enabled)
{
    set_favourite(state, FAVOURITE_POOL, "Pool", pool_spa_enabled);
    set_favourite(state, FAVOURITE_SPA, "Spa", pool_spa_enabled);
    set_favourite(state, 2, "filter only", true);
    set_favourite(state, 3, "filter and heatpump", true);
    set_favourite(state, 4, "Favourite 3", false);
    set_favourite(state, 5, "Favourite 4", false);
    set_favourite(state, 6, "Favourite 5", false);
    set_favourite(state, 7, "Favourite 6", false);
}

/* Publish the Favourite select's discovery and check its option list. */
static void check_favourite_options(const pool_state_t *state, const char *expected)
{
    unsigned before = publish_count;
    mqtt_publish_favourite_discovery_single(state);
    assert(publish_count == before + 1);
    assert(strcmp(last_topic, "homeassistant/select/test/pool_controller_A1B2C3_favourite/config") == 0);

    const char *options = strstr(last_payload, "\"options\":[");
    assert(options);
    options += strlen("\"options\":");
    const char *end = strchr(options, ']');
    assert(end);
    size_t length = (size_t)(end - options) + 1;
    if (strlen(expected) != length || strncmp(options, expected, length) != 0) {
        fprintf(stderr, "Favourite options\n  expected: %s\n  actual:   %.*s\n",
                expected, (int)length, options);
        exit(1);
    }
}

/* #156: with separate pool and spa modes off, only Pool and Spa are hidden.
 * Enabled user favourites stay available, as on the Touchscreen. */
static void test_separate_modes_off_keeps_user_favourites(void)
{
    static pool_state_t state;
    state.separate_pool_spa_modes_valid = true;
    state.separate_pool_spa_modes = false;
    set_reported_favourites(&state, false);
    check_favourite_options(&state,
        "[\"filter only\",\"filter and heatpump\",\"All Off\",\"All Auto\",\"No Favourite\"]");
}

static void test_separate_modes_on_adds_pool_and_spa(void)
{
    static pool_state_t state;
    state.separate_pool_spa_modes_valid = true;
    state.separate_pool_spa_modes = true;
    set_reported_favourites(&state, true);
    check_favourite_options(&state,
        "[\"Pool\",\"Spa\",\"filter only\",\"filter and heatpump\",\"All Off\",\"All Auto\",\"No Favourite\"]");
}

/* Hiding Pool and Spa waits until the Touchscreen has reported the setting. */
static void test_separate_modes_unknown_keeps_pool_and_spa(void)
{
    static pool_state_t state;
    set_reported_favourites(&state, true);
    check_favourite_options(&state,
        "[\"Pool\",\"Spa\",\"filter only\",\"filter and heatpump\",\"All Off\",\"All Auto\",\"No Favourite\"]");
}

/* An enabled favourite whose label hasn't arrived still gets offered. */
static void test_separate_modes_off_unnamed_favourite_uses_fallback(void)
{
    static pool_state_t state;
    state.separate_pool_spa_modes_valid = true;
    state.separate_pool_spa_modes = false;
    set_favourite(&state, 4, NULL, true);
    check_favourite_options(&state,
        "[\"Favourite 3\",\"All Off\",\"All Auto\",\"No Favourite\"]");
}

int main(void)
{
    test_separate_modes_off_keeps_user_favourites();
    test_separate_modes_on_adds_pool_and_spa();
    test_separate_modes_unknown_keeps_pool_and_spa();
    test_separate_modes_off_unnamed_favourite_uses_fallback();
    puts("MQTT discovery: Favourite select options passed");
    return 0;
}
