/**
 * Unit tests for the persisted "pump reports telemetry" flag (main/pump_telemetry.c).
 *
 * To run:
 *   cd test && gcc -I. -I.. -o run_pump_telemetry test_pump_telemetry.c ../main/pump_telemetry.c ../main/nvs_settings.c nvs_stub.c && ./run_pump_telemetry
 */

#include <stdio.h>
#include <stdbool.h>
#include "../main/pump_telemetry.h"
#include "nvs.h"

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

void test_not_seen_on_a_fresh_device(void)
{
    nvs_stub_reset();
    TEST_ASSERT(!pump_telemetry_load_seen(), "a device that has never seen a broadcast loads as not seen");
}

// A second load stands in for a reboot: it reads back what NVS kept.
void test_seen_survives_a_reboot(void)
{
    nvs_stub_reset();
    pump_telemetry_save_seen();
    TEST_ASSERT(pump_telemetry_load_seen(), "seen after a reboot, before any new broadcast");
}

void test_saving_again_keeps_it_seen(void)
{
    nvs_stub_reset();
    pump_telemetry_save_seen();
    pump_telemetry_save_seen();
    TEST_ASSERT(pump_telemetry_load_seen(), "saving twice still loads as seen");
}

int main(void)
{
    printf("\n======================================\n");
    printf("  Pump Telemetry Unit Tests\n");
    printf("======================================\n");

    test_not_seen_on_a_fresh_device();
    test_seen_survives_a_reboot();
    test_saving_again_keeps_it_seen();

    printf("\n======================================\n");
    printf("  Test Summary\n");
    printf("======================================\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("======================================\n\n");

    return (tests_failed == 0) ? 0 : 1;
}
