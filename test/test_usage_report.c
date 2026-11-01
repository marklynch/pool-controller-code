/**
 * Unit tests for anonymous usage reporting (main/usage_report.c).
 *
 * The HTTP client, RNG and app description are faked here, so the tests
 * can see every request the module makes.
 *
 * To run:
 *   cd test && gcc -I. -I.. -o run_usage_report test_usage_report.c ../main/usage_report.c ../main/nvs_settings.c log_capture.c nvs_stub.c && ./run_usage_report
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../main/usage_report.h"
#include "../main/config.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  ✓ PASS: %s\n", message); \
            tests_passed++; \
        } else { \
            printf("  ✗ FAIL: %s\n", message); \
            tests_failed++; \
        } \
    } while(0)

// ======================================================
// Fakes
// ======================================================

static uint8_t s_rng_seed;

// Each draw yields seed, seed+1, ... so a test can predict the ID, and
// changing the seed shows whether a new draw was taken.
void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(s_rng_seed + i);
}

static const esp_app_desc_t s_app = { .version = "1.8.2" };
const esp_app_desc_t *esp_app_get_description(void) { return &s_app; }

esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }

// One fake client at a time; the module never holds two.
static struct {
    int requests;              // esp_http_client_perform calls
    int open_clients;          // init minus cleanup
    char url[128];
    esp_http_client_method_t method;
    int timeout_ms;
    bool crt_bundle;
    char content_type[64];
    char user_agent[64];
    char body[128];
    esp_err_t perform_result;  // what the next perform returns
    int status_code;           // and the HTTP status it reports
} s_http;

static int s_handle_token;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    snprintf(s_http.url, sizeof(s_http.url), "%s", config->url);
    s_http.method = config->method;
    s_http.timeout_ms = config->timeout_ms;
    s_http.crt_bundle = (config->crt_bundle_attach != NULL);
    s_http.content_type[0] = s_http.user_agent[0] = s_http.body[0] = '\0';
    s_http.open_clients++;
    return (esp_http_client_handle_t)&s_handle_token;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    (void)client;
    if (strcmp(key, "Content-Type") == 0) snprintf(s_http.content_type, sizeof(s_http.content_type), "%s", value);
    if (strcmp(key, "User-Agent") == 0)   snprintf(s_http.user_agent, sizeof(s_http.user_agent), "%s", value);
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len)
{
    (void)client;
    snprintf(s_http.body, sizeof(s_http.body), "%.*s", len, data);
    return ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    (void)client;
    s_http.requests++;
    return s_http.perform_result;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    (void)client;
    return s_http.status_code;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    (void)client;
    s_http.open_clients--;
    return ESP_OK;
}

// ======================================================
// Helpers
// ======================================================

static void endpoint_ok(void)       { s_http.perform_result = ESP_OK;   s_http.status_code = 204; }
static void endpoint_down(void)     { s_http.perform_result = ESP_FAIL; s_http.status_code = 0; }
static void endpoint_5xx(void)      { s_http.perform_result = ESP_OK;   s_http.status_code = 503; }

// A device fresh from the factory: empty flash, healthy endpoint.
static void factory_fresh(uint8_t rng_seed)
{
    memset(&s_http, 0, sizeof(s_http));
    endpoint_ok();
    nvs_stub_reset();
    s_rng_seed = rng_seed;
    usage_report_init();
}

// Restart the device: flash survives, RAM doesn't. The RNG seed changes so a
// regenerated ID would be noticed.
static void reboot(void)
{
    s_rng_seed += 0x40;
    usage_report_init();
}

// The "id" value from the last request body.
static void last_id(char out[17])
{
    out[0] = '\0';
    const char *p = strstr(s_http.body, "\"id\":\"");
    if (p) snprintf(out, 17, "%s", p + 6);
}

// ======================================================
// Tests
// ======================================================

void test_request_shape(void)
{
    factory_fresh(0x10);
    usage_report_run(true);

    TEST_ASSERT(s_http.requests == 1, "First call after boot sends one request");
    TEST_ASSERT(strcmp(s_http.url, USAGE_REPORT_URL) == 0, "Posts to USAGE_REPORT_URL");
    TEST_ASSERT(s_http.method == HTTP_METHOD_POST, "Uses POST");
    TEST_ASSERT(s_http.timeout_ms == USAGE_REPORT_TIMEOUT_MS, "Uses the configured timeout");
    TEST_ASSERT(s_http.crt_bundle, "Verifies TLS against the certificate bundle");
    TEST_ASSERT(strcmp(s_http.content_type, "application/json") == 0, "Content-Type is application/json");
    TEST_ASSERT(strcmp(s_http.user_agent, "pool-controller-esp32/1.8.2") == 0, "User-Agent carries the version");
    TEST_ASSERT(strcmp(s_http.body, "{\"id\":\"1011121314151617\",\"v\":\"1.8.2\"}") == 0,
                "Body is exactly the install ID and the version");
    TEST_ASSERT(s_http.open_clients == 0, "HTTP client is cleaned up");
}

void test_enabled_by_default(void)
{
    factory_fresh(0x10);
    TEST_ASSERT(usage_report_get_enabled() == (USAGE_REPORT_DEFAULT_ON != 0),
                "A fresh device follows USAGE_REPORT_DEFAULT_ON");
}

// The ID comes from the RNG, lowercase hex, 16 characters.
void test_id_is_random_hex(void)
{
    factory_fresh(0xA0);
    usage_report_run(true);

    char id[17];
    last_id(id);
    TEST_ASSERT(strlen(id) == 16, "ID is 16 characters");
    TEST_ASSERT(strcmp(id, "a0a1a2a3a4a5a6a7") == 0, "ID is the RNG output as lowercase hex");
}

// Reboots and OTA updates keep NVS, so the same install must keep its ID.
void test_id_survives_reboot(void)
{
    factory_fresh(0x20);
    usage_report_run(true);
    char before[17];
    last_id(before);

    reboot();
    usage_report_run(true);
    char after[17];
    last_id(after);

    TEST_ASSERT(s_http.requests == 2, "Sends again after the reboot");
    TEST_ASSERT(strcmp(before, after) == 0, "ID is unchanged across a reboot");
}

// A factory reset erases NVS, which is expected to make a new install.
void test_factory_reset_makes_new_id(void)
{
    factory_fresh(0x30);
    usage_report_run(true);
    char before[17];
    last_id(before);

    factory_fresh(0x70);
    usage_report_run(true);
    char after[17];
    last_id(after);

    TEST_ASSERT(strcmp(before, after) != 0, "Erased flash gives a new ID");
}

// No rate limit of its own: the caller's schedule decides, so every
// scheduled run sends.
void test_every_scheduled_run_sends(void)
{
    factory_fresh(0x10);
    usage_report_run(true);
    usage_report_run(true);
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 3, "Each scheduled run sends one request");
}

// Runs off the schedule (a requested check, the opt-out wake) never ping.
void test_unscheduled_run_does_not_ping(void)
{
    factory_fresh(0x10);
    usage_report_run(false);
    usage_report_run(false);
    TEST_ASSERT(s_http.requests == 0, "No ping from an unscheduled run");

    usage_report_run(true);
    usage_report_run(false);
    TEST_ASSERT(s_http.requests == 1, "Still none once an ID exists");
}

// The opt-out wake is an unscheduled run, and must still send the deletion.
void test_unscheduled_run_sends_pending_delete(void)
{
    factory_fresh(0x10);
    usage_report_run(true);                    // ping creates the ID

    usage_report_set_enabled(false);
    usage_report_run(false);
    TEST_ASSERT(s_http.requests == 2 && s_http.method == HTTP_METHOD_DELETE,
                "Unscheduled run sends the pending DELETE");
}

// Opting out and straight back in before the wake runs: nothing is pending,
// and the wake must not turn into an extra ping.
void test_opt_out_then_in_wake_sends_nothing(void)
{
    factory_fresh(0x10);
    usage_report_run(true);
    usage_report_set_enabled(false);
    usage_report_set_enabled(true);
    usage_report_run(false);
    TEST_ASSERT(s_http.requests == 1, "Wake after opting back in sends nothing");
}

// Pings are fire-and-forget: one attempt per call, and a failed one isn't
// carried over to the next.
void test_failure_is_not_retried(void)
{
    factory_fresh(0x10);
    endpoint_down();
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 1, "Unreachable endpoint: exactly one attempt");
    TEST_ASSERT(s_http.open_clients == 0, "HTTP client cleaned up after a failure");

    endpoint_5xx();
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 2, "5xx: exactly one attempt");
    TEST_ASSERT(s_http.open_clients == 0, "HTTP client cleaned up after a 5xx");
}

void test_disabled_sends_nothing(void)
{
    factory_fresh(0x10);
    usage_report_set_enabled(false);
    usage_report_run(true);
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 0, "No request while disabled");

    reboot();
    TEST_ASSERT(usage_report_get_enabled() == false, "Disabled survives a reboot");
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 0, "Still no request after the reboot");

    usage_report_set_enabled(true);
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 1, "Sends as soon as it is re-enabled");

    reboot();
    TEST_ASSERT(usage_report_get_enabled() == true, "Re-enabled survives a reboot");
}

// Opting out of a device that has reported asks the server to delete its
// rows, once, and then goes quiet.
void test_opt_out_deletes_server_record(void)
{
    factory_fresh(0x10);
    usage_report_run(true);                        // ping creates the ID
    char id[17];
    last_id(id);

    usage_report_set_enabled(false);
    usage_report_run(true);

    char expected_url[128];
    snprintf(expected_url, sizeof(expected_url), USAGE_REPORT_URL "/%s", id);
    TEST_ASSERT(s_http.requests == 2, "Opting out sends one request");
    TEST_ASSERT(s_http.method == HTTP_METHOD_DELETE, "That request is a DELETE");
    TEST_ASSERT(strcmp(s_http.url, expected_url) == 0, "It names the install ID in the path");
    TEST_ASSERT(s_http.body[0] == '\0', "It carries no body");
    TEST_ASSERT(s_http.open_clients == 0, "HTTP client is cleaned up");

    usage_report_run(true);
    reboot();
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 2, "Nothing more is sent once the deletion is confirmed");
}

// The ID survives the deletion, so opting back in carries on with it.
void test_opt_in_after_delete_keeps_id(void)
{
    factory_fresh(0x10);
    usage_report_run(true);
    char before[17];
    last_id(before);

    usage_report_set_enabled(false);
    usage_report_run(true);                        // DELETE
    usage_report_set_enabled(true);
    usage_report_run(true);                        // ping
    char after[17];
    last_id(after);

    TEST_ASSERT(s_http.method == HTTP_METHOD_POST, "Re-enabling pings again");
    TEST_ASSERT(strcmp(before, after) == 0, "With the same install ID");
}

// The deletion is the one request that is retried, on each run and across a
// reboot, until the server confirms it.
void test_delete_retried_until_confirmed(void)
{
    factory_fresh(0x10);
    usage_report_run(true);

    usage_report_set_enabled(false);
    endpoint_down();
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 2, "Unreachable: one attempt per run");

    endpoint_5xx();
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 3, "5xx: tried again on the next run");

    reboot();
    endpoint_ok();
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 4 && s_http.method == HTTP_METHOD_DELETE,
                "Still pending after a reboot, and sent");

    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 4, "Confirmed: no more attempts");
}

// A device that never reported has nothing on the server to delete.
void test_opt_out_before_first_ping_sends_nothing(void)
{
    factory_fresh(0x10);
    usage_report_set_enabled(false);
    usage_report_run(true);
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 0, "No ID yet: no DELETE");
}

// Changing your mind before the deletion goes out withdraws it.
void test_opt_in_before_delete_cancels_it(void)
{
    factory_fresh(0x10);
    usage_report_run(true);
    usage_report_set_enabled(false);
    usage_report_set_enabled(true);
    usage_report_run(true);
    TEST_ASSERT(s_http.requests == 2 && s_http.method == HTTP_METHOD_POST,
                "Re-enabled before the next run: a ping, not a DELETE");
}

int main(void)
{
    printf("\n=== usage_report tests ===\n\n");

    test_request_shape();
    test_enabled_by_default();
    test_id_is_random_hex();
    test_id_survives_reboot();
    test_factory_reset_makes_new_id();
    test_every_scheduled_run_sends();
    test_unscheduled_run_does_not_ping();
    test_unscheduled_run_sends_pending_delete();
    test_opt_out_then_in_wake_sends_nothing();
    test_failure_is_not_retried();
    test_disabled_sends_nothing();
    test_opt_out_deletes_server_record();
    test_opt_in_after_delete_keeps_id();
    test_delete_retried_until_confirmed();
    test_opt_out_before_first_ping_sends_nothing();
    test_opt_in_before_delete_cancels_it();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
