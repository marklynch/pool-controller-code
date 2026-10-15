#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../main/heater_freshness_logic.h"

static int tests_passed;
static int tests_failed;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  ✓ PASS: %s\n", message); \
            tests_passed++; \
        } else { \
            printf("  ✗ FAIL: %s\n", message); \
            tests_failed++; \
        } \
    } while (0)

static pool_state_t make_online_heater_state(uint32_t last_response_ms)
{
    pool_state_t state;
    memset(&state, 0, sizeof(state));

    pool_heater_t *heater = &state.heaters[0];
    heater->valid = true;
    heater->on = true;
    heater->device_reported = true;
    heater->communications_online = true;
    heater->device_address = 0x0071;
    heater->device_last_response_ms = last_response_ms;
    heater->device_last_sample_epoch = 1791840000;
    heater->pool_setpoint_valid = true;
    heater->pool_setpoint = 28;
    heater->gas_heater_valid = true;
    heater->status = HEATER_SETPOINT_REACHED;

    state.num_seen_devices = 2;
    state.seen_devices[0].addr_hi = 0x00;
    state.seen_devices[0].addr_lo = 0x71;
    state.seen_devices[0].temp1_valid = true;
    state.seen_devices[0].temp1 = 35;
    state.seen_devices[1].addr_hi = 0x00;
    state.seen_devices[1].addr_lo = 0x62;
    state.seen_devices[1].temp1_valid = true;
    state.seen_devices[1].temp1 = 18;
    return state;
}

static void test_not_expired_before_timeout(void)
{
    pool_state_t state = make_online_heater_state(1000);
    uint32_t mask = heater_freshness_expire(&state, 180999, 180000);

    TEST_ASSERT(mask == 0, "heater remains online before timeout");
    TEST_ASSERT(state.heaters[0].communications_online,
                "communications flag remains online");
    TEST_ASSERT(state.heaters[0].gas_heater_valid,
                "gas status remains valid before timeout");
}

static void test_expiry_invalidates_only_direct_telemetry(void)
{
    pool_state_t state = make_online_heater_state(1000);
    uint32_t mask = heater_freshness_expire(&state, 181000, 180000);

    TEST_ASSERT(mask == 1U, "expired Heater 1 is returned in change mask");
    TEST_ASSERT(!state.heaters[0].communications_online,
                "heater communications become offline");
    TEST_ASSERT(!state.heaters[0].gas_heater_valid,
                "stale gas status is invalidated");
    TEST_ASSERT(!state.seen_devices[0].temp1_valid,
                "stale dedicated-heater temperature is invalidated");
    TEST_ASSERT(state.seen_devices[1].temp1_valid,
                "controller temperature remains valid");
    TEST_ASSERT(state.heaters[0].valid && state.heaters[0].on,
                "heater enable/request state is not fabricated or cleared");
    TEST_ASSERT(state.heaters[0].pool_setpoint_valid &&
                state.heaters[0].pool_setpoint == 28,
                "configuration setpoint remains valid");
    TEST_ASSERT(state.heaters[0].device_last_sample_epoch == 1791840000,
                "last real sample timestamp is preserved");
}

static void test_tick_wraparound(void)
{
    pool_state_t state = make_online_heater_state(UINT32_MAX - 99U);
    uint32_t mask = heater_freshness_expire(&state, 100U, 200U);

    TEST_ASSERT(mask == 1U, "timeout calculation survives tick wraparound");
}

int main(void)
{
    printf("\n======================================\n");
    printf("  Heater Freshness Unit Tests\n");
    printf("======================================\n");

    test_not_expired_before_timeout();
    test_expiry_invalidates_only_direct_telemetry();
    test_tick_wraparound();

    printf("\n  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
