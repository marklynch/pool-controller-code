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
void mqtt_publish_heater(const pool_state_t *state, int index) {}
void mqtt_publish_chlorinator(const pool_state_t *state) {}
void mqtt_publish_light(const pool_state_t *state, uint8_t zone) {}
void mqtt_publish_channel(const pool_state_t *state, uint8_t channel) {}
void mqtt_publish_valve(const pool_state_t *state, uint8_t valve_num) {}

// Mock register requester
void register_requester_notify(void) {}

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
 * Real message: 02 00 50 FF FF 80 00 17 10 F7 25 1D 63 54 F9 03
 * Payload: spa_c=0x25(37), pool_c=0x1D(29), spa_f=0x63(99), pool_f=0x54(84)
 * Byte[8]=0x10 (length=16), Byte[9]=0xF7 (header checksum = sum(bytes 0-8) & 0xFF)
 * Data checksum = (0x25+0x1D+0x63+0x54) & 0xFF = 0xF9
 */
void test_decode_temperature_setting(void)
{
    // Reset test state
    init_test_context();

    uint8_t msg[] = {
        0x02,                   // Byte 0: Start
        0x00, 0x50,             // Bytes 1-2: Source: Touch Screen
        0xFF, 0xFF,             // Bytes 3-4: Dest: Broadcast
        0x80, 0x00,             // Bytes 5-6: Control
        0x17, 0x10,             // Bytes 7-8: Command / length (16)
        0xF7,                   // Byte 9: Header checksum (sum bytes 0-8)
        0x25,                   // Byte 10: Spa setpoint 37°C
        0x1D,                   // Byte 11: Pool setpoint 29°C
        0x63,                   // Byte 12: Spa setpoint 99°F
        0x54,                   // Byte 13: Pool setpoint 84°F
        0xF9,                   // Byte 14: Data checksum
        0x03                    // Byte 15: End
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Temperature setting message should be decoded");
    TEST_ASSERT(test_pool_state.spa_setpoint == 37, "Spa setpoint should be 37°C");
    TEST_ASSERT(test_pool_state.pool_setpoint == 29, "Pool setpoint should be 29°C");
}

/**
 * Test: Heater status message (ON)
 * Real OFF message: 02 00 62 FF FF 80 00 12 0F 03 00 00 08 08 03
 * Byte[8]=0x0F (length=15), Byte[9]=0x03 (header checksum = sum(bytes 0-8) & 0xFF)
 * Heater state = payload[1]. ON = 0x01.
 * Data checksum = (0x00+0x01+0x08) & 0xFF = 0x09
 */
void test_decode_heater_on(void)
{
    // Reset test state
    init_test_context();

    uint8_t msg[] = {
        0x02,                   // Byte 0: Start
        0x00, 0x62,             // Bytes 1-2: Source: Temp Sensor
        0xFF, 0xFF,             // Bytes 3-4: Dest: Broadcast
        0x80, 0x00,             // Bytes 5-6: Control
        0x12, 0x0F,             // Bytes 7-8: Command / length (15)
        0x03,                   // Byte 9: Header checksum (sum bytes 0-8)
        0x00,                   // Byte 10: Payload[0]
        0x01,                   // Byte 11: Payload[1] = heater state ON
        0x08,                   // Byte 12: Payload[2]
        0x09,                   // Byte 13: Data checksum
        0x03                    // Byte 14: End
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Heater message should be decoded");
    TEST_ASSERT(test_pool_state.heaters[0].on, "Heater should be ON");
    TEST_ASSERT(test_pool_state.heaters[0].valid, "Heater valid flag should be set");
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
    TEST_ASSERT(strcmp(name, "Touch Screen") == 0, "0x0050 should be 'Touch Screen'");

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
 * Test: Heater status message (OFF)
 * Real message: 02 00 62 FF FF 80 00 12 0F 03 00 00 08 08 03
 */
void test_decode_heater_off(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00,
        0x12, 0x0F,  // Command / length (15)
        0x03,        // Header checksum
        0x00, 0x00, 0x08,  // Payload: state=0x00 (Off)
        0x08,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Heater OFF message should be decoded");
    TEST_ASSERT(!test_pool_state.heaters[0].on, "Heater should be OFF");
    TEST_ASSERT(test_pool_state.heaters[0].valid, "Heater valid flag should be set");
}

/**
 * Test: Current temperature reading
 * Real message: 02 00 62 FF FF 80 00 16 0E 06 1A 00 1A 03
 * Temperature = 0x1A = 26°C
 */
void test_decode_temp_reading(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00,
        0x16, 0x0E,  // Command / length (14)
        0x06,        // Header checksum (sum bytes 0-8 = 774, & 0xFF = 0x06)
        0x1A, 0x00,  // Payload: current_temp=26, unknown
        0x1A,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Temperature reading should be decoded");
    TEST_ASSERT(test_pool_state.current_temp == 26, "Current temperature should be 26°C");
    TEST_ASSERT(test_pool_state.temp_valid, "Temp valid flag should be set");
}

/**
 * Test: Channel status message — all channels off
 * Real message from bus capture (37 bytes):
 * 02 00 50 FF FF 80 00 0B 25 00 08 01 00 00 02 00 00 FE 00 00 FE 00 00 0B 00 00 09 00 00 FD 00 00 00 00 00 18 03
 * 8 channels: Filter/Off, Cleaning/Off, LightZone/Off, LightZone/Off, Jets/Off, Blower/Off, Heater/Off, Unused
 */
void test_decode_channel_status_all_off(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x0B, 0x25,  // Command / length (37)
        0x00,        // Header checksum (sum bytes 0-8 = 768, & 0xFF = 0x00)
        // payload[0]: num_channels = 8
        0x08,
        // Ch1: Filter(0x01), Off(0x00), Inactive(0x00)
        0x01, 0x00, 0x00,
        // Ch2: Cleaning(0x02), Off(0x00), Inactive(0x00)
        0x02, 0x00, 0x00,
        // Ch3: Light Zone(0xFE), Off(0x00), Inactive(0x00)
        0xFE, 0x00, 0x00,
        // Ch4: Light Zone(0xFE), Off(0x00), Inactive(0x00)
        0xFE, 0x00, 0x00,
        // Ch5: Jets(0x0B), Off(0x00), Inactive(0x00)
        0x0B, 0x00, 0x00,
        // Ch6: Blower(0x09), Off(0x00), Inactive(0x00)
        0x09, 0x00, 0x00,
        // Ch7: Heater(0xFD), Off(0x00), Inactive(0x00)
        0xFD, 0x00, 0x00,
        // Ch8: Unused(0x00)
        0x00, 0x00, 0x00,
        0x18,  // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Channel status message should be decoded");
    TEST_ASSERT(test_pool_state.num_channels == 8, "Should have 8 channels");

    TEST_ASSERT(test_pool_state.channels[0].configured, "Ch1 should be configured");
    TEST_ASSERT(test_pool_state.channels[0].type == 0x01, "Ch1 type should be Filter (0x01)");
    TEST_ASSERT(test_pool_state.channels[0].state == 0, "Ch1 state should be Off");
    TEST_ASSERT(!test_pool_state.channels[0].active, "Ch1 should be inactive");

    TEST_ASSERT(test_pool_state.channels[1].configured, "Ch2 should be configured");
    TEST_ASSERT(test_pool_state.channels[1].type == 0x02, "Ch2 type should be Cleaning (0x02)");

    TEST_ASSERT(test_pool_state.channels[2].configured, "Ch3 should be configured");
    TEST_ASSERT(test_pool_state.channels[2].type == 0xFE, "Ch3 type should be Light Zone (0xFE)");
    TEST_ASSERT(!test_pool_state.channels[2].active, "Ch3 should be inactive");

    TEST_ASSERT(test_pool_state.channels[6].configured, "Ch7 should be configured");
    TEST_ASSERT(test_pool_state.channels[6].type == 0xFD, "Ch7 type should be Heater (0xFD)");

    TEST_ASSERT(!test_pool_state.channels[7].configured, "Ch8 should be unconfigured (Unused)");
}

/**
 * Test: Channel status message — light zones active
 * Real message: 02 00 50 FF FF 80 00 0B 25 00 08 01 00 00 02 00 00 FE 02 01 FE 02 01 0B 00 00 09 00 00 FD 00 00 00 00 00 1E 03
 * Ch3 and Ch4 (Light Zone): On(0x02), Active(0x01)
 */
void test_decode_channel_status_lights_active(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x0B, 0x25, 0x00,
        0x08,
        0x01, 0x00, 0x00,  // Ch1: Filter, Off, Inactive
        0x02, 0x00, 0x00,  // Ch2: Cleaning, Off, Inactive
        0xFE, 0x02, 0x01,  // Ch3: Light Zone, On, Active
        0xFE, 0x02, 0x01,  // Ch4: Light Zone, On, Active
        0x0B, 0x00, 0x00,  // Ch5: Jets, Off, Inactive
        0x09, 0x00, 0x00,  // Ch6: Blower, Off, Inactive
        0xFD, 0x00, 0x00,  // Ch7: Heater, Off, Inactive
        0x00, 0x00, 0x00,  // Ch8: Unused
        0x1E,  // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Channel status (lights active) should be decoded");

    TEST_ASSERT(test_pool_state.channels[2].state == 2, "Ch3 state should be On (2)");
    TEST_ASSERT(test_pool_state.channels[2].active, "Ch3 should be active");

    TEST_ASSERT(test_pool_state.channels[3].state == 2, "Ch4 state should be On (2)");
    TEST_ASSERT(test_pool_state.channels[3].active, "Ch4 should be active");

    TEST_ASSERT(test_pool_state.channels[0].state == 0, "Ch1 should remain Off");
    TEST_ASSERT(!test_pool_state.channels[0].active, "Ch1 should remain inactive");
}

/**
 * Test: Chlorinator pH setpoint
 * Real message: 02 00 90 FF FF 80 00 1D 0F 3C 01 4E 00 4F 03
 * pH setpoint = UINT16_LE(payload[1..2]) = 0x004E = 78 (= 7.8 pH)
 */
void test_decode_chlor_ph_setpoint(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x90, 0xFF, 0xFF, 0x80, 0x00,
        0x1D, 0x0F,  // length (15)
        0x3C,        // Header checksum (sum bytes 0-8 = 828, & 0xFF = 0x3C)
        0x01,        // Sub-type: pH setpoint
        0x4E, 0x00,  // Value: 78 little-endian (pH 7.8)
        0x4F,        // Data checksum (0x01+0x4E+0x00 = 0x4F)
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Chlorinator pH setpoint should be decoded");
    TEST_ASSERT(test_pool_state.ph_setpoint == 78, "pH setpoint should be 78 (7.8 pH)");
}

/**
 * Test: Chlorinator ORP setpoint
 * Real message: 02 00 90 FF FF 80 00 1D 0F 3C 02 8A 02 8E 03
 * ORP setpoint = UINT16_LE(payload[1..2]) = 0x028A = 650 mV
 */
void test_decode_chlor_orp_setpoint(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x90, 0xFF, 0xFF, 0x80, 0x00,
        0x1D, 0x0F,  // length (15)
        0x3C,        // Header checksum
        0x02,        // Sub-type: ORP setpoint
        0x8A, 0x02,  // Value: 650 little-endian
        0x8E,        // Data checksum (0x02+0x8A+0x02 = 0x8E)
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Chlorinator ORP setpoint should be decoded");
    TEST_ASSERT(test_pool_state.orp_setpoint == 650, "ORP setpoint should be 650 mV");
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
    test_decode_temp_reading();
    test_decode_heater_on();
    test_decode_heater_off();

    // Channel status tests
    printf("\n--- Channel Status Tests ---\n");
    test_decode_channel_status_all_off();
    test_decode_channel_status_lights_active();

    // Chlorinator tests
    printf("\n--- Chlorinator Tests ---\n");
    test_decode_chlor_ph_setpoint();
    test_decode_chlor_orp_setpoint();

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
