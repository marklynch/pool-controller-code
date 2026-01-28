/**
 * Unit tests for message_decoder module
 *
 * These tests can be run without hardware using ESP-IDF's host-based testing
 * or a standard C test framework.
 *
 * To run: idf.py test
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../main/message_decoder.h"
#include "../main/pool_state.h"

// Mock FreeRTOS function implementations
uint32_t xTaskGetTickCount(void)
{
    return 1000;  // Mock tick count
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime)
{
    (void)xSemaphore;
    (void)xBlockTime;
    return pdTRUE;  // Always succeed in tests
}

void xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    (void)xSemaphore;
    // No-op in tests
}

// Mock MQTT functions (disabled in tests)
void mqtt_publish_mode(const pool_state_t *state) {}
void mqtt_publish_temperature(const pool_state_t *state) {}
void mqtt_publish_heater(const pool_state_t *state) {}
void mqtt_publish_chlorinator(const pool_state_t *state) {}
void mqtt_publish_light(const pool_state_t *state, uint8_t zone) {}
void mqtt_publish_channel(const pool_state_t *state, uint8_t channel) {}

// Test context
static pool_state_t test_pool_state;
static message_decoder_context_t test_ctx;
static int dummy_mutex = 0;  // Dummy mutex for testing

// Test counter
static int tests_passed = 0;
static int tests_failed = 0;

// Helper to initialize test context with dummy mutex
static void init_test_context(void)
{
    memset(&test_pool_state, 0, sizeof(test_pool_state));
    test_ctx.pool_state = &test_pool_state;
    test_ctx.state_mutex = (SemaphoreHandle_t)&dummy_mutex;
    test_ctx.enable_mqtt = false;
}

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("✓ PASS: %s\n", message); \
            tests_passed++; \
        } else { \
            printf("✗ FAIL: %s\n", message); \
            tests_failed++; \
        } \
    } while(0)

/**
 * Test: Checksum verification with valid message
 */
void test_checksum_valid(void)
{
    // Simple mode message with valid checksum
    // Checksum is the sum of data bytes (byte 10 only in this case)
    uint8_t msg[] = {
        0x02,               // Start
        0x00, 0x50,         // Source: Controller
        0xFF, 0xFF,         // Dest: Broadcast
        0x80, 0x00,         // Control
        0x14, 0x0D, 0xF1,   // Command (Mode message)
        0x01,               // Byte 10: Data (Mode = Pool)
        0x01,               // Byte 11: Checksum (sum of byte 10 = 0x01)
        0x03                // Byte 12: End
    };

    bool result = verify_message_checksum(msg, sizeof(msg));
    TEST_ASSERT(result, "Valid checksum should pass verification");
}

/**
 * Test: Checksum verification with invalid message
 */
void test_checksum_invalid(void)
{
    uint8_t msg[] = {
        0x02,               // Start
        0x00, 0x50,         // Source: Controller
        0xFF, 0xFF,         // Dest: Broadcast
        0x80, 0x00,         // Control
        0x14, 0x0D, 0xF1,   // Command
        0x01,               // Data
        0x00,               // Wrong checksum (should be 0xF2)
        0x03                // End
    };

    bool result = verify_message_checksum(msg, sizeof(msg));
    TEST_ASSERT(!result, "Invalid checksum should fail verification");
}

/**
 * Test: Checksum with message too short
 */
void test_checksum_too_short(void)
{
    uint8_t msg[] = {0x02, 0x00, 0x03};  // Too short

    bool result = verify_message_checksum(msg, sizeof(msg));
    TEST_ASSERT(!result, "Message too short should fail verification");
}

/**
 * Test: Mode message decoding (Spa mode)
 */
void test_decode_mode_spa(void)
{
    // Reset test state
    init_test_context();

    // Mode message: Spa mode (0x00)
    uint8_t msg[] = {
        0x02,                   // Start
        0x00, 0x50,             // Source: Controller
        0xFF, 0xFF,             // Dest: Broadcast
        0x80, 0x00,             // Control
        0x14, 0x0D, 0xF1,       // Command (Mode)
        0x00,                   // Byte 10: Data (Spa mode)
        0x00,                   // Byte 11: Checksum (sum of byte 10 = 0x00)
        0x03                    // Byte 12: End
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Mode message should be decoded");
    TEST_ASSERT(test_pool_state.mode == 0, "Mode should be set to Spa (0)");
    TEST_ASSERT(test_pool_state.mode_valid, "Mode valid flag should be set");
}

/**
 * Test: Mode message decoding (Pool mode)
 */
void test_decode_mode_pool(void)
{
    // Reset test state
    init_test_context();

    // Mode message: Pool mode (0x01)
    uint8_t msg[] = {
        0x02,                   // Start
        0x00, 0x50,             // Source: Controller
        0xFF, 0xFF,             // Dest: Broadcast
        0x80, 0x00,             // Control
        0x14, 0x0D, 0xF1,       // Command (Mode)
        0x01,                   // Byte 10: Data (Pool mode)
        0x01,                   // Byte 11: Checksum (sum of byte 10 = 0x01)
        0x03                    // Byte 12: End
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Mode message should be decoded");
    TEST_ASSERT(test_pool_state.mode == 1, "Mode should be set to Pool (1)");
    TEST_ASSERT(test_pool_state.mode_valid, "Mode valid flag should be set");
}

/**
 * Test: Temperature setting message
 */
void test_decode_temperature_setting(void)
{
    // Reset test state
    init_test_context();

    // Temperature setting message
    // Data starts at byte 10 (spa setpoint) and byte 11 (pool setpoint)
    uint8_t msg[] = {
        0x02,                   // Byte 0: Start
        0x00, 0x50,             // Bytes 1-2: Source: Controller
        0xFF, 0xFF,             // Bytes 3-4: Dest: Broadcast
        0x80, 0x00,             // Bytes 5-6: Control
        0x17, 0x10,             // Bytes 7-8: Command (Temperature setting)
        0x00,                   // Byte 9: Padding/extra data
        75,                     // Byte 10: Spa setpoint: 75°F
        80,                     // Byte 11: Pool setpoint: 80°F
        155,                    // Byte 12: Checksum (75 + 80 = 155 = 0x9B)
        0x03                    // Byte 13: End
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Temperature setting message should be decoded");
    TEST_ASSERT(test_pool_state.spa_setpoint == 75, "Spa setpoint should be 75");
    TEST_ASSERT(test_pool_state.pool_setpoint == 80, "Pool setpoint should be 80");
}

/**
 * Test: Heater status message
 */
void test_decode_heater_on(void)
{
    // Reset test state
    init_test_context();

    // Heater status message (ON)
    // Checksum is sum of data bytes from index 10 to (len-3)
    uint8_t msg[] = {
        0x02,                   // Byte 0: Start
        0x00, 0x62,             // Bytes 1-2: Source: Temp Sensor
        0xFF, 0xFF,             // Bytes 3-4: Dest: Broadcast
        0x80, 0x00,             // Bytes 5-6: Control
        0x12, 0x0F,             // Bytes 7-8: Command (Heater)
        0x00,                   // Byte 9: Padding
        0x00,                   // Byte 10: Data
        0x01,                   // Byte 11: Heater ON
        0x01,                   // Byte 12: Checksum (0x00 + 0x01 = 0x01)
        0x03                    // Byte 13: End
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Heater message should be decoded");
    TEST_ASSERT(test_pool_state.heater_on, "Heater should be ON");
    TEST_ASSERT(test_pool_state.heater_valid, "Heater valid flag should be set");
}

/**
 * Test: Malformed message (wrong start byte)
 */
void test_decode_malformed_start(void)
{
    init_test_context();

    uint8_t msg[] = {0xFF, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x03};  // Wrong start byte

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(!decoded, "Malformed message should not be decoded");
}

/**
 * Test: Malformed message (wrong end byte)
 */
void test_decode_malformed_end(void)
{
    init_test_context();

    uint8_t msg[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0xFF};  // Wrong end byte

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(!decoded, "Malformed message should not be decoded");
}

/**
 * Test: Device name lookup
 */
void test_device_name_lookup(void)
{
    const char *name;

    name = get_device_name(0x00, 0x50);
    TEST_ASSERT(strcmp(name, "Controller") == 0, "0x0050 should be 'Controller'");

    name = get_device_name(0x00, 0x62);
    TEST_ASSERT(strcmp(name, "Temp Sensor") == 0, "0x0062 should be 'Temp Sensor'");

    name = get_device_name(0x00, 0x90);
    TEST_ASSERT(strcmp(name, "Chlorinator") == 0, "0x0090 should be 'Chlorinator'");

    name = get_device_name(0xFF, 0xFF);
    TEST_ASSERT(strcmp(name, "Broadcast") == 0, "0xFFFF should be 'Broadcast'");

    name = get_device_name(0x12, 0x34);
    TEST_ASSERT(name == NULL, "Unknown address should return NULL");
}

/**
 * Run all tests
 */
int main(void)
{
    printf("\n");
    printf("======================================\n");
    printf("  Message Decoder Unit Tests\n");
    printf("======================================\n\n");

    // Checksum tests
    printf("--- Checksum Tests ---\n");
    test_checksum_valid();
    test_checksum_invalid();
    test_checksum_too_short();

    // Message decoding tests
    printf("\n--- Message Decoding Tests ---\n");
    test_decode_mode_spa();
    test_decode_mode_pool();
    test_decode_temperature_setting();
    test_decode_heater_on();

    // Malformed message tests
    printf("\n--- Malformed Message Tests ---\n");
    test_decode_malformed_start();
    test_decode_malformed_end();

    // Helper function tests
    printf("\n--- Helper Function Tests ---\n");
    test_device_name_lookup();

    // Summary
    printf("\n======================================\n");
    printf("  Test Summary\n");
    printf("======================================\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("======================================\n\n");

    return (tests_failed == 0) ? 0 : 1;
}
