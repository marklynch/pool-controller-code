/**
 * Unit tests for the filter pump type inference (main/filter_pump_type.c).
 *
 * To run:
 *   cd test && gcc -I. -I.. -o run_filter_pump_type test_filter_pump_type.c ../main/filter_pump_type.c nvs_stub.c && ./run_filter_pump_type
 */

#include <stdio.h>
#include <stdbool.h>
#include "../main/filter_pump_type.h"
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

// Wipe both the stored value and the module's RAM cache, so each test starts
// from a device that has never seen the pump run.
static void reset_all(void)
{
    nvs_stub_reset();
    filter_pump_type_init();
}

// Off and Auto are reported by single- and multi-speed channels alike, so they
// must leave the answer unknown rather than guessing either way.
void test_ambiguous_states_teach_nothing(void)
{
    reset_all();

    TEST_ASSERT(filter_pump_type_learn(0x00) == false, "Off reports no change");
    TEST_ASSERT(filter_pump_type_learn(0x01) == false, "Auto reports no change");
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_UNKNOWN,
                "Off/Auto leave the pump type unknown");
}

void test_speed_state_means_multi(void)
{
    reset_all();

    TEST_ASSERT(filter_pump_type_learn(0x04) == true, "Medium Speed reports a change");
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_MULTI_SPEED, "Medium Speed means multi-speed");
}

// The whole point of offering "On" while the answer is unknown: a multi-speed
// channel answers by reporting High Speed, a single-speed one by reporting On.
void test_plain_on_means_single(void)
{
    reset_all();

    TEST_ASSERT(filter_pump_type_learn(0x02) == true, "On reports a change");
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_SINGLE_SPEED, "On means single-speed");
}

void test_low_and_high_also_mean_multi(void)
{
    reset_all();
    filter_pump_type_learn(0x03);
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_MULTI_SPEED, "Low Speed means multi-speed");

    reset_all();
    filter_pump_type_learn(0x05);
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_MULTI_SPEED, "High Speed means multi-speed");
}

// Only a change should trigger the caller's discovery re-publish; a pump
// sitting at one speed broadcasts the same state every cycle.
void test_repeat_observation_is_not_a_change(void)
{
    reset_all();

    TEST_ASSERT(filter_pump_type_learn(0x05) == true, "first High Speed is a change");
    TEST_ASSERT(filter_pump_type_learn(0x05) == false, "repeated High Speed is not");
    TEST_ASSERT(filter_pump_type_learn(0x03) == false,
                "a different speed on an already-multi channel is not");
    TEST_ASSERT(filter_pump_type_learn(0x00) == false, "a later Off is not");
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_MULTI_SPEED,
                "and Off does not undo what was learned");
}

// A pump swap should correct itself rather than needing the flash cleared.
void test_contradiction_takes_the_newer_evidence(void)
{
    reset_all();
    filter_pump_type_learn(0x05);

    TEST_ASSERT(filter_pump_type_learn(0x02) == true, "multi -> single is a change");
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_SINGLE_SPEED, "and the newer answer wins");
}

// The reason for persisting at all: the evidence only appears while the pump
// runs, so a reboot with the channel Off must not throw the answer away.
void test_pump_type_survives_a_reboot(void)
{
    reset_all();
    filter_pump_type_learn(0x05);

    // Simulate a restart: RAM cache cleared, NVS intact.
    filter_pump_type_init();
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_MULTI_SPEED,
                "pump type is restored from NVS after a reboot");
}

void test_unknown_when_nothing_stored(void)
{
    nvs_stub_reset();
    filter_pump_type_init();
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_UNKNOWN,
                "a device that has never seen the pump run reports unknown");
}

int main(void)
{
    printf("\n======================================\n");
    printf("  Filter Pump Type Unit Tests\n");
    printf("======================================\n");

    printf("\n--- Inference Tests ---\n");
    test_unknown_when_nothing_stored();
    test_ambiguous_states_teach_nothing();
    test_speed_state_means_multi();
    test_plain_on_means_single();
    test_low_and_high_also_mean_multi();

    printf("\n--- Change Reporting Tests ---\n");
    test_repeat_observation_is_not_a_change();
    test_contradiction_takes_the_newer_evidence();

    printf("\n--- Persistence Tests ---\n");
    test_pump_type_survives_a_reboot();

    printf("\n======================================\n");
    printf("  Test Summary\n");
    printf("======================================\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("======================================\n\n");

    return (tests_failed == 0) ? 0 : 1;
}
