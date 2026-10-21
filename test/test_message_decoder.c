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
#include "../main/filter_pump_type.h"
#include "nvs.h"
#include "../main/unknown_buffer.h"

// From unknown_buffer_stub.c — counts frames flagged as undocumented
extern int unknown_buffer_record_count;

// Mock FreeRTOS function implementations
static uint32_t mock_ticks = 1000;
uint32_t xTaskGetTickCount(void)
{
    return mock_ticks;
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
void mqtt_publish_heater_setpoints(const pool_state_t *state, int index) {}
static unsigned temperature_publish_calls[2];
void mqtt_publish_temperature_reading(const pool_state_t *state, int dev_idx, uint8_t sensor_index)
{
    temperature_publish_calls[sensor_index - 1]++;
}
void mqtt_publish_heater(const pool_state_t *state, int index) {}
void mqtt_publish_gas_heater(const pool_state_t *state, int index) {}
static int heater_config_publishes = 0;
static int last_heater_config_index = -1;
void mqtt_publish_heater_config(const pool_state_t *state, int index)
{
    heater_config_publishes++;
    last_heater_config_index = index;
}
void mqtt_publish_chlorinator(const pool_state_t *state) {}
void mqtt_publish_pump(const pool_state_t *state) {}
void mqtt_publish_service_mode(const pool_state_t *state) {}
void mqtt_publish_light(const pool_state_t *state, uint8_t zone) {}
void mqtt_publish_channel(const pool_state_t *state, uint8_t channel) {}
void mqtt_publish_valve(const pool_state_t *state, uint8_t valve_num) {}
void mqtt_publish_favourite(const pool_state_t *state) {}

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
    mock_ticks = 1000;
    test_ctx.pool_state = &test_pool_state;
    test_ctx.state_mutex = (SemaphoreHandle_t)&dummy_mutex;
    test_ctx.enable_mqtt = false;
}

// Helper to set the heater configuration, as registers 0xC8 (Heater 1 Type)
// and 0xCC (Heater 2 Configured) report it
static void set_heater_config(uint8_t heater1_type, bool heater2_configured)
{
    test_pool_state.heater1_type             = heater1_type;
    test_pool_state.heater1_type_valid       = true;
    test_pool_state.heater2_configured       = heater2_configured;
    test_pool_state.heater2_configured_valid = true;
}

static bool decode_test_frame(uint8_t source, uint8_t command,
                              const uint8_t *payload, int payload_len)
{
    uint8_t message[64] = {0x02, 0x00, source, 0xFF, 0xFF, 0x80, 0x00, command};
    if (payload_len < 0 || payload_len > 52) return false;
    int length = payload_len + 12;
    message[8] = length;
    for (int i = 0; i < 9; i++) message[9] += message[i];
    memcpy(message + 10, payload, payload_len);
    for (int i = 0; i < payload_len; i++) message[length - 2] += payload[i];
    message[length - 1] = 0x03;
    return decode_message(message, length, &test_ctx);
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
    TEST_ASSERT(test_pool_state.heaters[0].spa_setpoint == 37, "Heater 1 spa setpoint should be 37°C");
    TEST_ASSERT(test_pool_state.heaters[0].pool_setpoint == 29, "Heater 1 pool setpoint should be 29°C");
    TEST_ASSERT(heater_setpoint_valid(&test_pool_state.heaters[0]), "Heater 1 setpoint should be valid");
}

/**
 * Test: Heater 2 setpoint registers (0xEA pool, 0xEB spa, slot 0x00) via CMD 0x38.
 */
void test_heater2_setpoint_registers(void)
{
    init_test_context();

    // Register 0xEA (Heater 2 pool setpoint) = 0x1B (27°C)
    // 02 00 50 FF FF 80 00 38 0F 17 EA 00 1B 05 03
    uint8_t pool_msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xEA, 0x00, 0x1B,       // reg 0xEA, slot 0x00, value 27°C
        0x05,                   // data checksum (0xEA+0x00+0x1B)
        0x03
    };
    bool pool_decoded = decode_message(pool_msg, sizeof(pool_msg), &test_ctx);
    TEST_ASSERT(pool_decoded, "Heater 2 pool setpoint register should be decoded");
    TEST_ASSERT(test_pool_state.heaters[1].pool_setpoint == 27, "Heater 2 pool setpoint should be 27°C");
    TEST_ASSERT(heater_setpoint_valid(&test_pool_state.heaters[1]), "Heater 2 setpoint should be valid");

    // Register 0xEB (Heater 2 spa setpoint) = 0x18 (24°C)
    // checksum = (0xEB+0x00+0x18) & 0xFF = 0x03
    uint8_t spa_msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xEB, 0x00, 0x18,       // reg 0xEB, slot 0x00, value 24°C
        0x03,                   // data checksum (0xEB+0x00+0x18)
        0x03
    };
    bool spa_decoded = decode_message(spa_msg, sizeof(spa_msg), &test_ctx);
    TEST_ASSERT(spa_decoded, "Heater 2 spa setpoint register should be decoded");
    TEST_ASSERT(test_pool_state.heaters[1].spa_setpoint == 24, "Heater 2 spa setpoint should be 24°C");
}

/**
 * Test: heater device setpoint broadcast (CMD 0x17 from 0x0074 ICI Gas Heater).
 * Real message: 02 00 74 FF FF 80 00 17 0E 19 1D 11 2E 03
 * Payload: spa_c=0x1D(29), pool_c=0x11(17)
 * On a single-heater install the values land in Heater 1.
 */
void test_heater_device_setpoint_broadcast(void)
{
    init_test_context();
    set_heater_config(HEATER1_TYPE_GENUS, false);

    uint8_t msg[] = {
        0x02,                   // Byte 0: Start
        0x00, 0x74,             // Bytes 1-2: Source: ICI Gas Heater
        0xFF, 0xFF,             // Bytes 3-4: Dest: Broadcast
        0x80, 0x00,             // Bytes 5-6: Control
        0x17, 0x0E,             // Bytes 7-8: Command / length (14)
        0x19,                   // Byte 9: Header checksum
        0x1D,                   // Byte 10: Spa setpoint 29°C
        0x11,                   // Byte 11: Pool setpoint 17°C
        0x2E,                   // Byte 12: Data checksum
        0x03                    // Byte 13: End
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Heater device setpoint broadcast should be decoded");
    TEST_ASSERT(test_pool_state.heaters[0].spa_setpoint == 29, "Heater 1 spa setpoint should be 29°C");
    TEST_ASSERT(test_pool_state.heaters[0].pool_setpoint == 17, "Heater 1 pool setpoint should be 17°C");
    TEST_ASSERT(test_pool_state.heaters[0].spa_setpoint_f == 84, "Heater 1 spa setpoint should be 84°F");
    TEST_ASSERT(test_pool_state.heaters[0].pool_setpoint_f == 62, "Heater 1 pool setpoint should be 62°F");
    TEST_ASSERT(heater_setpoint_valid(&test_pool_state.heaters[0]), "Heater 1 setpoint should be valid");
    TEST_ASSERT(test_pool_state.heaters[0].spa_setpoint_valid, "Heater 1 spa setpoint should be valid");
    TEST_ASSERT(test_pool_state.heaters[0].pool_setpoint_valid, "Heater 1 pool setpoint should be valid");
}

void test_viron_heater_setpoint_broadcast(void)
{
    init_test_context();
    set_heater_config(HEATER1_TYPE_GENUS, false);

    uint8_t msg[] = {
        0x02, 0x00, 0x71, 0xFF, 0xFF, 0x80, 0x00,
        0x17, 0x0E, 0x16,
        0x25,                   // Spa setpoint 37°C
        0x1B,                   // Pool setpoint 27°C
        0x40,
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded, "Viron setpoint broadcast should be decoded");
    TEST_ASSERT(h->spa_setpoint_valid && h->spa_setpoint == 37,
                "Viron spa setpoint should be 37°C");
    TEST_ASSERT(h->pool_setpoint_valid && h->pool_setpoint == 27,
                "Viron pool setpoint should be 27°C");
}

/**
 * Test: heater device setpoint broadcast carrying the 0x0A (10°C) "not plumbed
 * to this circuit" default. That circuit must be left alone; the other is applied.
 */
void test_heater_device_setpoint_unplumbed_circuit(void)
{
    init_test_context();
    set_heater_config(HEATER1_TYPE_GENUS, false);

    // A spa setpoint already known from register 0xE8 must survive the broadcast
    test_pool_state.heaters[0].spa_setpoint       = 29;
    test_pool_state.heaters[0].spa_setpoint_valid = true;

    uint8_t msg[] = {
        0x02, 0x00, 0x74, 0xFF, 0xFF, 0x80, 0x00,
        0x17, 0x0E, 0x19,
        0x0A,                   // Byte 10: Spa setpoint 10°C — unplumbed default
        0x11,                   // Byte 11: Pool setpoint 17°C
        0x1B,                   // Byte 12: Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Heater device setpoint broadcast should be decoded");
    TEST_ASSERT(test_pool_state.heaters[0].spa_setpoint == 29, "Known spa setpoint should survive the 10°C default");
    TEST_ASSERT(test_pool_state.heaters[0].pool_setpoint == 17, "Heater 1 pool setpoint should be 17°C");
}

/**
 * Test: heater device setpoint broadcast before register 0xCC has been
 * received. Nothing is assumed about a second heater, even with Heater 2's
 * registers seen, so the broadcast must not write Heater 1 — the register
 * broadcasts stay authoritative.
 */
void test_heater_device_setpoint_config_unknown(void)
{
    init_test_context();

    // Register 0xEA (Heater 2 pool setpoint) = 0x1B (27°C)
    uint8_t reg_msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xEA, 0x00, 0x1B,
        0x05,
        0x03
    };
    decode_message(reg_msg, sizeof(reg_msg), &test_ctx);

    uint8_t msg[] = {
        0x02, 0x00, 0x74, 0xFF, 0xFF, 0x80, 0x00,
        0x17, 0x0E, 0x19,
        0x1D,                   // Byte 10: Spa setpoint 29°C
        0x11,                   // Byte 11: Pool setpoint 17°C
        0x2E,                   // Byte 12: Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Heater device setpoint broadcast should be decoded");
    TEST_ASSERT(!heater_setpoint_valid(&test_pool_state.heaters[0]), "Heater 1 setpoint should stay unset before 0xCC arrives");
    TEST_ASSERT(test_pool_state.heaters[0].pool_setpoint == 0, "Heater 1 pool setpoint should not be written");
    TEST_ASSERT(test_pool_state.heaters[0].spa_setpoint == 0, "Heater 1 spa setpoint should not be written");
}

/**
 * Test: heater device setpoint broadcast after the second heater is removed.
 * Heater 2's registers have been seen, but register 0xCC now says no second
 * heater is configured, so the broadcast belongs to Heater 1.
 */
void test_heater_device_setpoint_heater2_removed(void)
{
    init_test_context();

    // Register 0xEA (Heater 2 pool setpoint) = 0x1B (27°C) — from before the removal
    uint8_t reg_msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xEA, 0x00, 0x1B,
        0x05,
        0x03
    };
    decode_message(reg_msg, sizeof(reg_msg), &test_ctx);

    // Register 0xCC (Heater 2 Configured) = 0x00
    uint8_t cc_msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xCC, 0x00, 0x00,
        0xCC,
        0x03
    };
    decode_message(cc_msg, sizeof(cc_msg), &test_ctx);

    uint8_t msg[] = {
        0x02, 0x00, 0x74, 0xFF, 0xFF, 0x80, 0x00,
        0x17, 0x0E, 0x19,
        0x1D,                   // Byte 10: Spa setpoint 29°C
        0x11,                   // Byte 11: Pool setpoint 17°C
        0x2E,                   // Byte 12: Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Heater device setpoint broadcast should be decoded");
    TEST_ASSERT(test_pool_state.heaters[0].spa_setpoint == 29, "Heater 1 spa setpoint should be 29°C");
    TEST_ASSERT(test_pool_state.heaters[0].pool_setpoint == 17, "Heater 1 pool setpoint should be 17°C");
}

/**
 * Test: heater device setpoint broadcast while register 0xCC says a second
 * heater is configured, before anything else about Heater 2 has arrived. The
 * broadcast cannot be attributed to a slot, so Heater 1 is left alone.
 */
void test_heater_device_setpoint_heater2_configured(void)
{
    init_test_context();

    // Register 0xCC (Heater 2 Configured) = 0x01
    uint8_t cc_msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xCC, 0x00, 0x01,
        0xCD,
        0x03
    };
    decode_message(cc_msg, sizeof(cc_msg), &test_ctx);

    uint8_t msg[] = {
        0x02, 0x00, 0x74, 0xFF, 0xFF, 0x80, 0x00,
        0x17, 0x0E, 0x19,
        0x1D,                   // Byte 10: Spa setpoint 29°C
        0x11,                   // Byte 11: Pool setpoint 17°C
        0x2E,                   // Byte 12: Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Heater device setpoint broadcast should be decoded");
    TEST_ASSERT(!heater_setpoint_valid(&test_pool_state.heaters[0]), "Heater 1 setpoint should stay unset with a second heater configured");
}

/**
 * Test: the heater configuration registers are stored in pool state.
 * 0xC8 = 0x03 (Heater 1 Connect Inbuilt), 0xCC = 0x01 (second heater configured).
 */
void test_heater_config_registers(void)
{
    init_test_context();

    uint8_t c8_msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xC8, 0x00, 0x03,
        0xCB,
        0x03
    };
    uint8_t cc_msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xCC, 0x00, 0x01,
        0xCD,
        0x03
    };

    TEST_ASSERT(decode_message(c8_msg, sizeof(c8_msg), &test_ctx), "Heater 1 Type register should be decoded");
    TEST_ASSERT(decode_message(cc_msg, sizeof(cc_msg), &test_ctx), "Heater 2 Configured register should be decoded");
    TEST_ASSERT(test_pool_state.heater1_type_valid, "Heater 1 type should be valid");
    TEST_ASSERT(test_pool_state.heater1_type == HEATER1_TYPE_CONNECT_INBUILT, "Heater 1 type should be Connect Inbuilt");
    TEST_ASSERT(test_pool_state.heater2_configured_valid, "Heater 2 configured should be valid");
    TEST_ASSERT(test_pool_state.heater2_configured, "Heater 2 should be configured");
}

/**
 * Test: heater_configured() reports each slot from the heater configuration
 * registers, and stays unknown until its register has arrived.
 */
void test_heater_configured_helper(void)
{
    init_test_context();
    bool known = true;

    TEST_ASSERT(!heater_configured(&test_pool_state, 0, &known) && !known,
                "Heater 1 should be unknown before 0xC8 arrives");
    TEST_ASSERT(!heater_configured(&test_pool_state, 1, &known) && !known,
                "Heater 2 should be unknown before 0xCC arrives");

    // 0xC8 = 0x00 (no Heater 1), 0xCC = 0x00 (no second heater)
    uint8_t c8_none[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xC8, 0x00, 0x00,
        0xC8,
        0x03
    };
    uint8_t cc_none[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xCC, 0x00, 0x00,
        0xCC,
        0x03
    };
    decode_message(c8_none, sizeof(c8_none), &test_ctx);
    decode_message(cc_none, sizeof(cc_none), &test_ctx);
    TEST_ASSERT(!heater_configured(&test_pool_state, 0, &known) && known,
                "Heater 1 should be known and not configured with 0xC8 = 0x00");
    TEST_ASSERT(!heater_configured(&test_pool_state, 1, &known) && known,
                "Heater 2 should be known and not configured with 0xCC = 0x00");

    // 0xC8 = 0x01 (Genus), 0xCC = 0x01 (second heater configured)
    uint8_t c8_genus[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xC8, 0x00, 0x01,
        0xC9,
        0x03
    };
    uint8_t cc_yes[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xCC, 0x00, 0x01,
        0xCD,
        0x03
    };
    decode_message(c8_genus, sizeof(c8_genus), &test_ctx);
    decode_message(cc_yes, sizeof(cc_yes), &test_ctx);
    TEST_ASSERT(heater_configured(&test_pool_state, 0, &known) && known,
                "Heater 1 should be configured with 0xC8 = 0x01 (Genus)");
    TEST_ASSERT(heater_configured(&test_pool_state, 1, &known) && known,
                "Heater 2 should be configured with 0xCC = 0x01");
}

/**
 * Test: a heater configuration register triggers mqtt_publish_heater_config
 * when it first arrives and when its value changes, not on every repeat.
 */
void test_heater_config_change_publishes(void)
{
    init_test_context();
    test_ctx.enable_mqtt = true;
    heater_config_publishes = 0;
    last_heater_config_index = -1;

    uint8_t c8_inbuilt[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xC8, 0x00, 0x03,
        0xCB,
        0x03
    };
    uint8_t c8_none[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xC8, 0x00, 0x00,
        0xC8,
        0x03
    };
    uint8_t cc_none[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x0F, 0x17,
        0xCC, 0x00, 0x00,
        0xCC,
        0x03
    };

    decode_message(c8_inbuilt, sizeof(c8_inbuilt), &test_ctx);
    TEST_ASSERT(heater_config_publishes == 1 && last_heater_config_index == 0,
                "First 0xC8 should publish Heater 1's configuration");

    decode_message(c8_inbuilt, sizeof(c8_inbuilt), &test_ctx);
    TEST_ASSERT(heater_config_publishes == 1, "Repeated 0xC8 with the same value should not publish");

    decode_message(c8_none, sizeof(c8_none), &test_ctx);
    TEST_ASSERT(heater_config_publishes == 2 && last_heater_config_index == 0,
                "Changed 0xC8 should publish Heater 1's configuration");

    decode_message(cc_none, sizeof(cc_none), &test_ctx);
    TEST_ASSERT(heater_config_publishes == 3 && last_heater_config_index == 1,
                "First 0xCC should publish Heater 2's configuration");

    test_ctx.enable_mqtt = false;
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
    set_heater_config(HEATER1_TYPE_CONNECT_INBUILT, false);

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
 *
 * Signature: get_device_name(hi, lo, fallback_buf, buf_size). Known
 * addresses return a static label; unknown addresses are formatted into the
 * caller's buffer as "Unknown 0xHHLL".
 */
void test_device_name_lookup(void)
{
    char buf[16];
    const char *name;

    name = get_device_name(0x00, 0x50, buf, sizeof(buf));
    TEST_ASSERT(strcmp(name, "Touch Screen") == 0, "0x0050 should be 'Touch Screen'");

    name = get_device_name(0x00, 0x62, buf, sizeof(buf));
    TEST_ASSERT(strcmp(name, "Connect 8/10") == 0, "0x0062 should be 'Connect 8/10'");

    name = get_device_name(0x00, 0x90, buf, sizeof(buf));
    TEST_ASSERT(strcmp(name, "RolaChem") == 0, "0x0090 should be 'RolaChem'");

    // Named from the Touchscreen's device table; the next model in the same
    // family has no name there, so it stays unknown.
    name = get_device_name(0x00, 0xD1, buf, sizeof(buf));
    TEST_ASSERT(strcmp(name, "Solar Roof Sensor") == 0, "0x00D1 should be 'Solar Roof Sensor'");

    name = get_device_name(0x00, 0xD2, buf, sizeof(buf));
    TEST_ASSERT(strcmp(name, "Unknown 0x00D2") == 0, "0x00D2 should be unknown");

    name = get_device_name(0xFF, 0xFF, buf, sizeof(buf));
    TEST_ASSERT(strcmp(name, "Broadcast") == 0, "0xFFFF should be 'Broadcast'");

    name = get_device_name(0x12, 0x34, buf, sizeof(buf));
    TEST_ASSERT(strcmp(name, "Unknown 0x1234") == 0,
                "Unknown address should format as 'Unknown 0xHHLL'");
}

/**
 * Test: Heater status message (OFF)
 * Real message: 02 00 62 FF FF 80 00 12 0F 03 00 00 08 08 03
 */
void test_decode_heater_off(void)
{
    init_test_context();
    set_heater_config(HEATER1_TYPE_CONNECT_INBUILT, false);

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
 * Test: the Connect 8/10's heater bit is not used before register 0xC8 has
 * been received — nothing is assumed about Heater 1. Service mode still is.
 */
void test_decode_heater_bit_ignored_until_type_known(void)
{
    init_test_context();

    // 02 00 62 ... 12 0F 03 00 03 08 0B 03 — heater ON, service mode ON
    uint8_t msg[] = {
        0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00,
        0x12, 0x0F,
        0x03,
        0x00, 0x03, 0x08,
        0x0B,
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Controller status should be decoded");
    TEST_ASSERT(!test_pool_state.heaters[0].valid, "Heater 1 should stay unset before 0xC8 arrives");
    TEST_ASSERT(test_pool_state.service_mode_valid && test_pool_state.service_mode,
                "Service mode should still be set");
}

/**
 * Test: with a Genus (smart-control) Heater 1, the heater's own status is
 * authoritative and the Connect 8/10's heater bit is ignored. Seen on two
 * installs: the controller's bit stays 0 while the heater runs.
 */
void test_decode_heater_bit_ignored_for_genus(void)
{
    init_test_context();
    set_heater_config(HEATER1_TYPE_GENUS, false);

    // HiNRG Gas Heater (0x0072) status 0x01 — heater on
    uint8_t heater_msg[] = {
        0x02, 0x00, 0x72, 0xFF, 0xFF, 0x80, 0x00,
        0x12, 0x10,
        0x14,
        0x00, 0x01, 0x00, 0x00,
        0x01,
        0x03
    };
    // Connect 8/10 status with the heater bit clear: 02 00 62 ... 12 0F 03 04 00 00 04 03
    uint8_t controller_msg[] = {
        0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00,
        0x12, 0x0F,
        0x03,
        0x04, 0x00, 0x00,
        0x04,
        0x03
    };

    decode_message(heater_msg, sizeof(heater_msg), &test_ctx);
    decode_message(controller_msg, sizeof(controller_msg), &test_ctx);

    TEST_ASSERT(test_pool_state.heaters[0].valid && test_pool_state.heaters[0].on,
                "Heater 1 should stay ON from the Genus heater's own status");
}

/**
 * Test: Current temperature reading
 * Real message: 02 00 62 FF FF 80 00 16 0E 06 1A 00 1A 03
 * Temperature = 0x1A = 26°C
 *
 * Per-source readings now live on the seen_device_t entry indexed by source
 * address — there is no longer a top-level current_temp on pool_state.
 */
void test_decode_temp_reading(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00,
        0x16, 0x0E,  // Command / length (14)
        0x06,        // Header checksum (sum bytes 0-8 = 774, & 0xFF = 0x06)
        0x1A, 0x00,  // Payload: temp1=26, temp2=0
        0x1A,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Temperature reading should be decoded");
    TEST_ASSERT(test_pool_state.num_seen_devices >= 1, "Source should be registered");
    TEST_ASSERT(test_pool_state.seen_devices[0].addr_hi == 0x00 &&
                test_pool_state.seen_devices[0].addr_lo == 0x62,
                "First seen device should be 0x0062 (Connect 8/10)");
    TEST_ASSERT(test_pool_state.seen_devices[0].temp1 == 26,
                "Sensor 1 temperature should be 26°C");
    TEST_ASSERT(test_pool_state.seen_devices[0].temp1_valid,
                "Sensor 1 valid flag should be set");
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
    TEST_ASSERT(test_pool_state.channels[0].type == CHANNEL_TYPE_FILTER, "Ch1 type should be Filter (0x01)");
    TEST_ASSERT(test_pool_state.channels[0].state == 0, "Ch1 state should be Off");
    TEST_ASSERT(!test_pool_state.channels[0].active, "Ch1 should be inactive");

    TEST_ASSERT(test_pool_state.channels[1].configured, "Ch2 should be configured");
    TEST_ASSERT(test_pool_state.channels[1].type == CHANNEL_TYPE_CLEANING, "Ch2 type should be Cleaning (0x02)");

    TEST_ASSERT(test_pool_state.channels[2].configured, "Ch3 should be configured");
    TEST_ASSERT(test_pool_state.channels[2].type == CHANNEL_TYPE_LIGHT_ZONE, "Ch3 type should be Light Zone (0xFE)");
    TEST_ASSERT(!test_pool_state.channels[2].active, "Ch3 should be inactive");

    TEST_ASSERT(test_pool_state.channels[6].configured, "Ch7 should be configured");
    TEST_ASSERT(test_pool_state.channels[6].type == CHANNEL_TYPE_HEATER, "Ch7 type should be Heater (0xFD)");

    TEST_ASSERT(!test_pool_state.channels[7].configured, "Ch8 should be unconfigured (Unused)");
}

void test_channel_status_resets_pump_speed_when_filter_off(void)
{
    init_test_context();

    // Setup an existing pump speed. pump_telemetry_seen goes with it: a
    // non-zero speed can only have come from handle_pump_speed, which sets
    // both, and the zeroing below is gated on it.
    test_pool_state.pump_speed = 1500;
    test_pool_state.pump_speed_valid = true;
    test_pool_state.pump_telemetry_seen = true;

    // Broadcast Channel Status with Filter (Ch1) in Auto mode (0x01) but Inactive (0x00)
    uint8_t msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x0B, 0x25,  // Command / length (37)
        0x00,        // Header checksum 
        0x01,        // payload[0]: num_channels = 1
        0x01, 0x01, 0x00, // Ch1: Filter(0x01), Auto(0x01), Inactive(0x00)
        0x01, 0x01, 0x00, // Ch1: Filter(0x01), Auto(0x01), Inactive(0x00)
        0x00, 0x00, 0x00,  // Ch2: Unused
        0x00, 0x00, 0x00,  // Ch3: Unused
        0x00, 0x00, 0x00,  // Ch4: Unused
        0x00, 0x00, 0x00,  // Ch5: Unused
        0x00, 0x00, 0x00,  // Ch6: Unused
        0x00, 0x00, 0x00,  // Ch7: Unused
        0x00, 0x00, 0x00,  // Ch8: Unused
        0x03,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Channel status message should be decoded");
    
    // Pump speed should be reset to 0 since Filter went to Off
    TEST_ASSERT(test_pool_state.pump_speed_valid, "pump_speed_valid should be true");
    TEST_ASSERT(test_pool_state.pump_speed == 0, "pump_speed should be 0");
}

// An install with a plain single-speed pump never broadcasts CMD 0x3B, so
// pump_telemetry_seen stays false. The Filter-inactive zeroing must not fire
// there: marking speed/power valid would invent readings out of nothing and
// publish HA pump entities that can only ever read 0.
void test_channel_status_does_not_fabricate_pump_speed_without_telemetry(void)
{
    init_test_context();

    // No pump telemetry has ever been decoded
    test_pool_state.pump_speed = 0;
    test_pool_state.pump_speed_valid = false;
    test_pool_state.pump_power_watts_valid = false;
    test_pool_state.pump_telemetry_seen = false;

    // Broadcast Channel Status with Filter (Ch1) in Auto mode (0x01) but Inactive (0x00)
    uint8_t msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x0B, 0x25,  // Command / length (37)
        0x00,        // Header checksum 
        0x01,        // payload[0]: num_channels = 1
        0x01, 0x01, 0x00, // Ch1: Filter(0x01), Auto(0x01), Inactive(0x00)
        0x00, 0x00, 0x00,  // Ch2: Unused
        0x00, 0x00, 0x00,  // Ch3: Unused
        0x00, 0x00, 0x00,  // Ch4: Unused
        0x00, 0x00, 0x00,  // Ch5: Unused
        0x00, 0x00, 0x00,  // Ch6: Unused
        0x00, 0x00, 0x00,  // Ch7: Unused
        0x00, 0x00, 0x00,  // Ch8: Unused
        0x03,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Channel status message should be decoded");
    
    // Both readings must stay invalid — there is no pump reporting them
    TEST_ASSERT(!test_pool_state.pump_speed_valid,
                "pump_speed_valid should stay false with no telemetry ever seen");
    TEST_ASSERT(!test_pool_state.pump_power_watts_valid,
                "pump_power_watts_valid should stay false with no telemetry ever seen");
    TEST_ASSERT(!test_pool_state.pump_telemetry_seen,
                "channel status should never set pump_telemetry_seen");
}

void test_decode_connect10_dedicated_solar_channel(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x0B, 0x25, 0x00,
        0x08,
        0x09, 0x01, 0x00,  // Ch1 Blower Auto inactive
        0xFC, 0x00, 0x00,  // Ch2 dedicated Solar raw tuple
        0x11, 0x01, 0x00,  // Ch3 Heater Power Auto inactive
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x01, 0x01, 0x01,  // Ch8 Filter Auto active
        0x23, 0x03
    };

    TEST_ASSERT(decode_message(msg, sizeof(msg), &test_ctx),
                "Connect 10 dedicated Solar frame should decode");
    TEST_ASSERT(test_pool_state.channels[1].type == CHANNEL_TYPE_SOLAR_CONTROLLER,
                "Channel 2 type 0xFC should be dedicated Solar");
    TEST_ASSERT(strcmp(get_channel_type_name(test_pool_state.channels[1].type), "Solar Controller") == 0,
                "Dedicated Solar should be labelled Solar Controller");
}

void test_dedicated_solar_uses_solar_status_mode_and_activity(void)
{
    init_test_context();

    // Learn that channel 2 is the dedicated Solar controller. Its raw 0x0B
    // tuple says Off/inactive even when the separate Solar status says otherwise.
    uint8_t channel_status[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x0B, 0x25, 0x00,
        0x08,
        0x09, 0x01, 0x00,
        0xFC, 0x00, 0x00,
        0x11, 0x01, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x01, 0x01, 0x01,
        0x23, 0x03
    };
    TEST_ASSERT(decode_message(channel_status, sizeof(channel_status), &test_ctx),
                "Dedicated Solar baseline channel status decoded");

    uint8_t solar_auto[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x2C, 0x12, 0x0E,
        0x23, 0x01, 0x00, 0x00, 0x04, 0x07, 0x2F, 0x03
    };
    TEST_ASSERT(decode_message(solar_auto, sizeof(solar_auto), &test_ctx),
                "Inactive Auto Solar status decoded");
    TEST_ASSERT(test_pool_state.solar_mode_valid && test_pool_state.solar_mode == 1,
                "Solar mode retained from CMD 0x2C");
    TEST_ASSERT(test_pool_state.channels[1].state == 1,
                "Dedicated Solar channel reports Auto from CMD 0x2C");
    TEST_ASSERT(!test_pool_state.solar_active_valid,
                "CMD 0x2C does not invent an output sample");
    uint8_t stopped[] = {0, 0, 0};
    TEST_ASSERT(decode_test_frame(0x62, 0x12, stopped, 3), "Stopped controller output decoded");
    TEST_ASSERT(test_pool_state.solar_active_valid && !test_pool_state.solar_active,
                "Inactive output comes from controller status");

    // This configuration byte also stays 0x02 with the pump stopped.
    // It cannot establish activity; the independent controller bit must do so.
    uint8_t solar_running[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x2C, 0x12, 0x0E,
        0x2B, 0x01, 0x02, 0x00, 0x02, 0x04, 0x34, 0x03
    };
    TEST_ASSERT(decode_message(solar_running, sizeof(solar_running), &test_ctx),
                "Running Solar status decoded");
    TEST_ASSERT(!test_pool_state.solar_active,
                "Nonzero CMD 0x2C configuration does not activate Solar");
    uint8_t running[] = {0, 4, 0};
    TEST_ASSERT(decode_test_frame(0x62, 0x12, running, 3), "Running controller output decoded");
    TEST_ASSERT(test_pool_state.solar_active && test_pool_state.channels[1].active,
                "Dedicated Solar activity comes from 0x0062 CMD 0x12 bit 2");

    TEST_ASSERT(decode_message(channel_status, sizeof(channel_status), &test_ctx),
                "Following dedicated Solar channel status decoded");
    TEST_ASSERT(test_pool_state.channels[1].state == 1 && test_pool_state.channels[1].active,
                "Stale 0x0B Solar tuple cannot overwrite mode or activity");

    TEST_ASSERT(decode_message(solar_auto, sizeof(solar_auto), &test_ctx),
                "Solar stop status decoded");
    TEST_ASSERT(test_pool_state.channels[1].active,
                "Zero CMD 0x2C configuration does not clear actual activity");
    TEST_ASSERT(decode_test_frame(0x62, 0x12, stopped, 3), "Stopped output decoded");
    TEST_ASSERT(!test_pool_state.channels[1].active,
                "Dedicated Solar clears only from controller output status");
}

/**
 * Test: the Channel State register (0x8C+N) cannot overwrite a dedicated Solar
 * channel's separately retained mode and output — its register value is as stale as its 0x0B tuple.
 */
void test_dedicated_solar_state_register_does_not_override(void)
{
    init_test_context();
    uint8_t output[] = {0, 4, 0};
    TEST_ASSERT(decode_test_frame(0x62, 0x12, output, 3), "Actual running output received");

    // Channel 2 type register = 0xFC (dedicated Solar)
    uint8_t type_reg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x0F, 0x17,
        0x6D, 0x02, 0xFC, 0x6B, 0x03
    };
    // CMD 0x2C: Auto, unknown configuration byte 0x02
    uint8_t solar_running[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x2C, 0x12, 0x0E,
        0x2B, 0x01, 0x02, 0x00, 0x02, 0x04, 0x34, 0x03
    };
    // Channel 2 state register = 0x00 (Off), as in the periodic register dump
    uint8_t state_reg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x0F, 0x17,
        0x8D, 0x02, 0x00, 0x8F, 0x03
    };

    TEST_ASSERT(decode_message(type_reg, sizeof(type_reg), &test_ctx),
                "Solar channel type register decoded");
    TEST_ASSERT(decode_message(solar_running, sizeof(solar_running), &test_ctx),
                "Running Solar status decoded");
    TEST_ASSERT(decode_message(state_reg, sizeof(state_reg), &test_ctx),
                "Solar channel state register decoded");
    TEST_ASSERT(test_pool_state.channels[1].state == 1 && test_pool_state.channels[1].active,
                "Channel state register cannot overwrite dedicated Solar mode");
}

/**
 * Test: a CMD 0x2C that arrives before the Solar channel is known is applied
 * once the channel type register reveals it.
 */
void test_dedicated_solar_status_before_type_register(void)
{
    init_test_context();
    uint8_t output[] = {0, 4, 0};
    TEST_ASSERT(decode_test_frame(0x62, 0x12, output, 3), "Actual running output received");

    uint8_t solar_running[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x2C, 0x12, 0x0E,
        0x2B, 0x01, 0x02, 0x00, 0x02, 0x04, 0x34, 0x03
    };
    uint8_t type_reg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x0F, 0x17,
        0x6D, 0x02, 0xFC, 0x6B, 0x03
    };

    TEST_ASSERT(decode_message(solar_running, sizeof(solar_running), &test_ctx),
                "Running Solar status decoded before channel known");
    TEST_ASSERT(decode_message(type_reg, sizeof(type_reg), &test_ctx),
                "Solar channel type register decoded");
    TEST_ASSERT(test_pool_state.channels[1].configured &&
                test_pool_state.channels[1].state == 1 && test_pool_state.channels[1].active,
                "Retained mode/output applied when Solar channel type is learned");
}

/**
 * Test: an undocumented CMD 0x2C mode is flagged and not retained, but the
 * independent controller output sample is retained.
 */
void test_dedicated_solar_undocumented_mode_keeps_active(void)
{
    init_test_context();
    uint8_t output[] = {0, 4, 0};
    TEST_ASSERT(decode_test_frame(0x62, 0x12, output, 3), "Actual running output received");

    uint8_t type_reg[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x0F, 0x17,
        0x6D, 0x02, 0xFC, 0x6B, 0x03
    };
    // Mode 0x03 (undocumented), unknown configuration byte 0x02
    uint8_t solar_odd_mode[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x2C, 0x12, 0x0E,
        0x23, 0x03, 0x02, 0x00, 0x04, 0x07, 0x33, 0x03
    };

    TEST_ASSERT(decode_message(type_reg, sizeof(type_reg), &test_ctx),
                "Solar channel type register decoded");
    int before = unknown_buffer_record_count;
    TEST_ASSERT(decode_message(solar_odd_mode, sizeof(solar_odd_mode), &test_ctx),
                "Solar status with undocumented mode decoded");
    TEST_ASSERT(unknown_buffer_record_count > before,
                "Undocumented Solar mode is flagged");
    TEST_ASSERT(!test_pool_state.solar_mode_valid,
                "Undocumented Solar mode is not retained");
    TEST_ASSERT(test_pool_state.channels[1].active,
                "Independent output state preserved despite undocumented mode");
}

/**
 * Test: an undocumented raw 0x0B state on the dedicated Solar channel is still
 * flagged, even though CMD 0x2C's mode replaces it.
 */
void test_dedicated_solar_undocumented_raw_state_flagged(void)
{
    init_test_context();

    uint8_t solar_auto[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x2C, 0x12, 0x0E,
        0x23, 0x01, 0x00, 0x00, 0x04, 0x07, 0x2F, 0x03
    };
    // Channel status with the Solar tuple FC 07 00 (state 0x07 undocumented)
    uint8_t channel_status[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x0B, 0x25, 0x00,
        0x08,
        0x09, 0x01, 0x00,
        0xFC, 0x07, 0x00,
        0x11, 0x01, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x01, 0x01, 0x01,
        0x2A, 0x03
    };

    TEST_ASSERT(decode_message(solar_auto, sizeof(solar_auto), &test_ctx),
                "Auto Solar status decoded");
    int before = unknown_buffer_record_count;
    TEST_ASSERT(decode_message(channel_status, sizeof(channel_status), &test_ctx),
                "Channel status with odd Solar state decoded");
    TEST_ASSERT(unknown_buffer_record_count > before,
                "Undocumented raw Solar state is flagged");
    TEST_ASSERT(test_pool_state.channels[1].state == 1,
                "CMD 0x2C mode still replaces raw Solar state");
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
 * Test: Channel status message — multi-speed pump channel (extended states)
 * Real messages from ninkasi bus capture: a Filter channel driving the Viron XT
 * pump reports On at Medium (0x04) then High (0x05) instead of plain On (0x02):
 * 02 00 50 FF FF 80 00 0B 25 00 08 01 04 01 04 00 00 FB 00 00 ... 1F 03
 * 02 00 50 FF FF 80 00 0B 25 00 08 01 05 01 04 00 00 FB 00 00 ... 20 03
 */

// The MQTT pump-mode select's option list is driven by what the decoder learns
// here, so the wiring from a 0x0B broadcast into filter_pump_type_learn needs
// covering in its own right — the module's unit tests exercise the inference,
// not the fact that handle_channel_status actually feeds it.
void test_channel_status_teaches_filter_pump_type(void)
{
    init_test_context();
    nvs_stub_reset();
    filter_pump_type_init();

    // Ch1 Filter in Auto: common to both pump kinds, so it settles nothing.
    uint8_t msg_auto[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x0B, 0x25, 0x00,
        0x01,              // num_channels = 1
        0x01, 0x01, 0x00,  // Ch1: Filter, Auto, Inactive
        0x00, 0x00, 0x00,  0x00, 0x00, 0x00,  0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,  0x00, 0x00, 0x00,  0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,  0x00, 0x00, 0x00,
        0x03, 0x03
    };
    TEST_ASSERT(decode_message(msg_auto, sizeof(msg_auto), &test_ctx),
                "Channel status (Auto) should be decoded");
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_UNKNOWN,
                "Auto should leave the filter pump type unknown");

    // Ch1 Filter at High: only a multi-speed channel reports 0x03-0x05.
    uint8_t msg_high[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x0B, 0x25, 0x00,
        0x01,              // num_channels = 1
        0x01, 0x05, 0x01,  // Ch1: Filter, High, Active
        0x00, 0x00, 0x00,  0x00, 0x00, 0x00,  0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,  0x00, 0x00, 0x00,  0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,  0x00, 0x00, 0x00,
        0x08, 0x03
    };
    TEST_ASSERT(decode_message(msg_high, sizeof(msg_high), &test_ctx),
                "Channel status (High) should be decoded");
    TEST_ASSERT(filter_pump_type_get() == FILTER_PUMP_TYPE_MULTI_SPEED,
                "A High Filter state should teach multi-speed");
}

// The select publishes CHANNEL_STATE_NAMES as its options and reads the state
// topic back against them, so any state the decoder can record for a channel
// has to have a name in that table.
void test_channel_state_names_cover_every_state(void)
{
    for (int i = 0; i < CHANNEL_STATE_COUNT; i++) {
        TEST_ASSERT(CHANNEL_STATE_NAMES[i] != NULL && CHANNEL_STATE_NAMES[i][0] != '\0',
                    "Every channel state code has a name");
    }
}

void test_decode_channel_status_multispeed_pump(void)
{
    init_test_context();

    uint8_t msg_med[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x0B, 0x25, 0x00,
        0x08,
        0x01, 0x04, 0x01,  // Ch1: Filter, On - Medium Speed (0x04), Active
        0x04, 0x00, 0x00,  // Ch2: Booster, Off, Inactive
        0xFE, 0x00, 0x00,  // Ch3: Light Zone, Off, Inactive
        0xFB, 0x00, 0x00,  // Ch4: Secondary Heater, Off, Inactive
        0x00, 0x00, 0x00,  // Ch5: Unused
        0x00, 0x00, 0x00,  // Ch6: Unused
        0x0A, 0x00, 0x00,  // Ch7: Swimjet, Off, Inactive
        0x0A, 0x00, 0x00,  // Ch8: Swimjet, Off, Inactive
        0x1F,  // Data checksum
        0x03
    };

    bool decoded = decode_message(msg_med, sizeof(msg_med), &test_ctx);

    TEST_ASSERT(decoded, "Channel status (multi-speed Medium) should be decoded");
    TEST_ASSERT(test_pool_state.channels[0].type == CHANNEL_TYPE_FILTER, "Ch1 type should be Filter (0x01)");
    TEST_ASSERT(test_pool_state.channels[0].state == 4, "Ch1 state should be On - Medium Speed (4)");
    TEST_ASSERT(test_pool_state.channels[0].active, "Ch1 should be active");

    uint8_t msg_high[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x0B, 0x25, 0x00,
        0x08,
        0x01, 0x05, 0x01,  // Ch1: Filter, On - High Speed (0x05), Active
        0x04, 0x00, 0x00,
        0xFE, 0x00, 0x00,
        0xFB, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
        0x0A, 0x00, 0x00,
        0x0A, 0x00, 0x00,
        0x20,  // Data checksum
        0x03
    };

    decoded = decode_message(msg_high, sizeof(msg_high), &test_ctx);

    TEST_ASSERT(decoded, "Channel status (multi-speed High) should be decoded");
    TEST_ASSERT(test_pool_state.channels[0].state == 5, "Ch1 state should be On - High Speed (5)");
    TEST_ASSERT(test_pool_state.channels[0].active, "Ch1 should be active");
}

/**
 * Test: Channel toggle command from the Internet Gateway (CMD 0x10)
 * Real message: 02 00 F0 FF FF 80 00 10 0D 8D 04 04 03
 */
void test_decode_channel_toggle_gateway(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x10, 0x0D,  // Command / length (13)
        0x8D,        // Header checksum
        0x04,        // Channel index 4 (Channel 5)
        0x04,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Gateway channel toggle should be decoded");
}

/**
 * Test: Channel toggle command broadcast by the Connect 10 controller
 * when a channel button is pressed on the controller itself (CMD 0x10)
 * Real message: 02 00 62 FF FF 80 00 10 0D FF 07 07 03
 */
void test_decode_channel_toggle_controller(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00,
        0x10, 0x0D,  // Command / length (13)
        0xFF,        // Header checksum
        0x07,        // Channel index 7 (Channel 8)
        0x07,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Controller channel toggle should be decoded");
}

/**
 * Test: Register write command from the Internet Gateway (CMD 0x3A) —
 * light zone 2 state -> On
 * Real message: 02 00 F0 FF FF 80 00 3A 0F B9 C1 01 02 C4 03
 */
void test_decode_register_write_gateway(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F,  // Command / length (15)
        0xB9,        // Header checksum
        0xC1,        // Register 0xC1 (Light Zone 2 state)
        0x01,        // Slot 0x01
        0x02,        // Value (On)
        0xC4,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Gateway register write should be decoded");
}

/**
 * Test: Viron Chlorinator unknown countdown (CMD 0x38, reg 0xE9, slot 0x0B),
 * answering the Internet Gateway's 0x39 poll — decoded, flagged undocumented
 * Real payload: E9 0B 9C 31 00 00 (12700 units = 42 min 20 s)
 */
void test_decode_chlor_unknown_countdown(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x84, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x12,  // Command / length (18)
        0x4E,        // Header checksum
        0xE9,        // Register 0xE9
        0x0B,        // Slot 0x0B
        0x9C, 0x31,  // 12700, little-endian
        0x00, 0x00,  // Unknown, always 00 00
        0xC1,        // Data checksum
        0x03
    };

    int before = unknown_buffer_record_count;
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Chlorinator unknown countdown should be decoded");
    TEST_ASSERT(unknown_buffer_record_count > before,
                "Chlorinator unknown countdown should be recorded as undocumented");
}

/**
 * Test: Viron Chlorinator unknown value (CMD 0x38, reg 0xEA, slot 0x0B) —
 * decoded, flagged undocumented
 * Real payload: EA 0B 28 23 00 00 (9000)
 */
void test_decode_chlor_unknown_value(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x84, 0xFF, 0xFF, 0x80, 0x00,
        0x38, 0x12,  // Command / length (18)
        0x4E,        // Header checksum
        0xEA,        // Register 0xEA
        0x0B,        // Slot 0x0B
        0x28, 0x23,  // 9000, little-endian
        0x00, 0x00,  // Unknown, always 00 00
        0x40,        // Data checksum
        0x03
    };

    int before = unknown_buffer_record_count;
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Chlorinator unknown value should be decoded");
    TEST_ASSERT(unknown_buffer_record_count > before,
                "Chlorinator unknown value should be recorded as undocumented");
}

/**
 * Test: Register write command from the Viron Chlorinator (CMD 0x3A) —
 * light zone 1 state -> On, sent by the chlorinator's own app
 * Real message: 02 00 84 FF FF 80 00 3A 0F 4D C0 01 02 C3 03
 */
void test_decode_register_write_chlorinator(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x84, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F,  // Command / length (15)
        0x4D,        // Header checksum
        0xC0,        // Register 0xC0 (Light Zone 1 state)
        0x01,        // Slot 0x01
        0x02,        // Value (On)
        0xC3,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Chlorinator register write should be decoded");
}

/**
 * Test: Register write command from the Viron Chlorinator (CMD 0x3A) —
 * light zone 1 color write
 * Real message: 02 00 84 FF FF 80 00 3A 0F 4D D0 01 0D DE 03
 */
void test_decode_register_write_light_color(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x84, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F,  // Command / length (15)
        0x4D,        // Header checksum
        0xD0,        // Register 0xD0 (Light Zone 1 color)
        0x01,        // Slot 0x01
        0x0D,        // Color code
        0xDE,        // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Light zone color write should be decoded");
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
 * Test: Chlorinator pump mode unicast
 * Real message: 02 00 84 00 50 80 00 0F 0E 73 01 03 04 03 (Low)
 * Pump mode = payload[1] = 0x03
 */
void test_decode_chlor_set_pump_mode(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x84, 0x00, 0x50, 0x80, 0x00,
        0x0F, 0x0E,  // command 0x0F, length (14)
        0x73,        // Header checksum
        0x01,        // fixed byte 10
        0x03,        // pump speed: 0x03 = Low
        0x04,        // Data checksum (0x01+0x03 = 0x04)
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Chlorinator pump mode should be decoded");
}

/**
 * Test: Chlorinator pump mode unicast — unexpected payload[0]
 * Real message pattern with payload[0]=0x00 instead of 0x01; should still be
 * recognised (decoded=true) and logged at WARN rather than treated as unknown.
 */
void test_decode_chlor_set_pump_mode_bad_byte0(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x84, 0x00, 0x50, 0x80, 0x00,
        0x0F, 0x0E,
        0x73,
        0x00,        // unexpected: should be 0x01
        0x03,        // Low
        0x03,        // Data checksum (0x00+0x03 = 0x03)
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Chlorinator pump mode with unexpected payload[0] should still be decoded");
}

/**
 * Test: Chlorinator pump mode unicast — out-of-range mode value
 * payload[1]=0x06 is above the known range (0x00–0x05); should still be
 * recognised and not counted as an unknown message.
 */
void test_decode_chlor_set_pump_mode_bad_mode(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0x84, 0x00, 0x50, 0x80, 0x00,
        0x0F, 0x0E,
        0x73,
        0x01,
        0x06,        // out-of-range mode
        0x07,        // Data checksum (0x01+0x06 = 0x07)
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Chlorinator pump mode with out-of-range mode should still be decoded");
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
 * Test: Pump speed — 1125 RPM
 * Real message from log: 02 00 A0 FF FF 80 00 3B 0E 69 04 65 69 03
 * Payload bytes 10-11 big-endian: 0x04 0x65 = 1125
 */
void test_decode_pump_speed_1125(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02,                   // Start
        0x00, 0xA0,             // Source: Viron XT Pump
        0xFF, 0xFF,             // Dest: Broadcast
        0x80, 0x00,             // Control
        0x3B, 0x0E,             // CMD 0x3B, LEN 14
        0x69,                   // Header checksum (sum bytes 0-8 = 873, & 0xFF = 0x69)
        0x04, 0x65,             // Speed: big-endian 0x0465 = 1125 RPM
        0x69,                   // Data checksum (0x04+0x65 = 0x69)
        0x03                    // End
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Pump speed 1125 RPM should be decoded");
    TEST_ASSERT(test_pool_state.pump_speed_valid, "pump_speed_valid should be set");
    TEST_ASSERT(test_pool_state.pump_speed == 1125, "pump_speed should be 1125 RPM");
}

/**
 * Test: Pump speed — 1350 RPM
 * Real message from log: 02 00 A0 FF FF 80 00 3B 0E 69 05 46 4B 03
 * Payload bytes 10-11 big-endian: 0x05 0x46 = 1350
 */
void test_decode_pump_speed_1350(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0xA0, 0xFF, 0xFF, 0x80, 0x00,
        0x3B, 0x0E, 0x69,
        0x05, 0x46,   // 0x0546 = 1350 RPM
        0x4B,         // Data checksum (0x05+0x46 = 0x4B)
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Pump speed 1350 RPM should be decoded");
    TEST_ASSERT(test_pool_state.pump_speed_valid, "pump_speed_valid should be set");
    TEST_ASSERT(test_pool_state.pump_speed == 1350, "pump_speed should be 1350 RPM");
}

/**
 * Test: Pump speed — 1500 RPM
 * Real message from log: 02 00 A0 FF FF 80 00 3B 0E 69 05 DC E1 03
 * Payload bytes 10-11 big-endian: 0x05 0xDC = 1500
 */
void test_decode_pump_speed_1500(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0xA0, 0xFF, 0xFF, 0x80, 0x00,
        0x3B, 0x0E, 0x69,
        0x05, 0xDC,   // 0x05DC = 1500 RPM
        0xE1,         // Data checksum (0x05+0xDC = 0xE1)
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Pump speed 1500 RPM should be decoded");
    TEST_ASSERT(test_pool_state.pump_speed_valid, "pump_speed_valid should be set");
    TEST_ASSERT(test_pool_state.pump_speed == 1500, "pump_speed should be 1500 RPM");
}

/**
 * Test: Pump speed — 0 RPM (pump stopped/transitioning)
 * Real message from log: 02 00 A0 FF FF 80 00 3B 0E 69 00 00 00 03
 */
void test_decode_pump_speed_zero(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0xA0, 0xFF, 0xFF, 0x80, 0x00,
        0x3B, 0x0E, 0x69,
        0x00, 0x00,   // 0 RPM
        0x00,         // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Pump speed 0 RPM should be decoded");
    TEST_ASSERT(test_pool_state.pump_speed_valid, "pump_speed_valid should be set even for 0 RPM");
    TEST_ASSERT(test_pool_state.pump_speed == 0, "pump_speed should be 0 RPM");
}

/**
 * Test: Pump button activity (CMD 0x1B) — decoded but no state change
 * Real message from log: 02 00 A0 FF FF 80 00 1B 0D 48 01 01 03
 */
void test_decode_pump_activity(void)
{
    init_test_context();

    uint8_t msg[] = {
        0x02, 0x00, 0xA0, 0xFF, 0xFF, 0x80, 0x00,
        0x1B, 0x0D,   // CMD 0x1B, LEN 13
        0x48,         // Header checksum (sum bytes 0-8 = 840, & 0xFF = 0x48)
        0x01,         // Activity value 0x01
        0x01,         // Data checksum
        0x03
    };

    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);

    TEST_ASSERT(decoded, "Pump activity message should be decoded (log-only)");
    TEST_ASSERT(!test_pool_state.pump_speed_valid,
                "pump_speed_valid should remain unset after activity-only message");
}

// ======================================================
// Gas Heater Status Tests (CMD 0x12)
//
// ICI frame:   02 00 74 FF FF 80 00 12 10 16 00 SS 00 00 SS 03
// HiNRG frame: 02 00 72 FF FF 80 00 12 10 14 00 SS 00 00 SS 03
// Data checksum = SS (payload[1] only; all other payload bytes are 0x00)
// ======================================================

static void build_ici_gas_heater_msg(uint8_t status_byte, uint8_t *buf)
{
    buf[0]  = 0x02; buf[1]  = 0x00; buf[2]  = 0x74;
    buf[3]  = 0xFF; buf[4]  = 0xFF; buf[5]  = 0x80;
    buf[6]  = 0x00; buf[7]  = 0x12; buf[8]  = 0x10;
    buf[9]  = 0x16; // header checksum
    buf[10] = 0x00; buf[11] = status_byte;
    buf[12] = 0x00; buf[13] = 0x00;
    buf[14] = status_byte; // data checksum = sum of payload bytes
    buf[15] = 0x03;
}

void test_gas_heater_ici_off(void)
{
    init_test_context();
    uint8_t msg[16];
    build_ici_gas_heater_msg(0x00, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "0x00: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "0x00: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_OFF,               "0x00: status=HEATER_OFF");
    TEST_ASSERT(!h->on,                                "0x00: on=false");
    TEST_ASSERT(!h->water_flow_detected,               "0x00: no water flow");
    TEST_ASSERT(!h->locked_out,                        "0x00: not locked out");
    TEST_ASSERT(h->burner_state == BURNER_OFF,         "0x00: burner off");
}

void test_gas_heater_ici_no_flow(void)
{
    init_test_context();
    uint8_t msg[16];
    build_ici_gas_heater_msg(0x01, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "0x01: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "0x01: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_ON_NO_FLOW,        "0x01: status=HEATER_ON_NO_FLOW");
    TEST_ASSERT(h->on,                                 "0x01: on=true");
    TEST_ASSERT(!h->water_flow_detected,               "0x01: no water flow");
    TEST_ASSERT(!h->locked_out,                        "0x01: not locked out");
    TEST_ASSERT(h->burner_state == BURNER_OFF,         "0x01: burner off");
}

void test_gas_heater_ici_off_with_flow(void)
{
    init_test_context();
    uint8_t msg[16];
    build_ici_gas_heater_msg(0x02, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "0x02: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "0x02: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_OFF,               "0x02: status=HEATER_OFF");
    TEST_ASSERT(!h->on,                                "0x02: on=false");
    TEST_ASSERT(h->water_flow_detected,                "0x02: water flow detected");
    TEST_ASSERT(!h->locked_out,                        "0x02: not locked out");
    TEST_ASSERT(h->burner_state == BURNER_OFF,         "0x02: burner off");
}

void test_gas_heater_ici_setpoint_reached(void)
{
    init_test_context();
    uint8_t msg[16];
    build_ici_gas_heater_msg(0x03, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "0x03: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "0x03: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_SETPOINT_REACHED,  "0x03: status=HEATER_SETPOINT_REACHED");
    TEST_ASSERT(h->on,                                 "0x03: on=true");
    TEST_ASSERT(h->water_flow_detected,                "0x03: water flow detected");
    TEST_ASSERT(!h->locked_out,                        "0x03: not locked out");
    TEST_ASSERT(h->burner_state == BURNER_OFF,         "0x03: burner off");
}

void test_gas_heater_ici_igniting(void)
{
    init_test_context();
    uint8_t msg[16];
    build_ici_gas_heater_msg(0x07, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "0x07: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "0x07: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_IGNITING,          "0x07: status=HEATER_IGNITING");
    TEST_ASSERT(h->on,                                 "0x07: on=true");
    TEST_ASSERT(h->water_flow_detected,                "0x07: water flow detected");
    TEST_ASSERT(!h->locked_out,                        "0x07: not locked out");
    TEST_ASSERT(h->burner_state == BURNER_IGNITING,    "0x07: burner igniting");
}

void test_gas_heater_ici_heating(void)
{
    init_test_context();
    uint8_t msg[16];
    build_ici_gas_heater_msg(0x0F, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "0x0F: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "0x0F: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_HEATING,           "0x0F: status=HEATER_HEATING");
    TEST_ASSERT(h->on,                                 "0x0F: on=true");
    TEST_ASSERT(h->water_flow_detected,                "0x0F: water flow detected");
    TEST_ASSERT(!h->locked_out,                        "0x0F: not locked out");
    TEST_ASSERT(h->burner_state == BURNER_ALIGHT,      "0x0F: burner alight");
}

void test_gas_heater_ici_cooldown(void)
{
    init_test_context();
    uint8_t msg[16];
    build_ici_gas_heater_msg(0x12, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "0x12: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "0x12: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_COOLDOWN,          "0x12: status=HEATER_COOLDOWN");
    TEST_ASSERT(!h->on,                                "0x12: on=false");
    TEST_ASSERT(h->water_flow_detected,                "0x12: water flow detected");
    TEST_ASSERT(h->locked_out,                         "0x12: locked out");
    TEST_ASSERT(h->burner_state == BURNER_OFF,         "0x12: burner off");
}

void test_gas_heater_ici_locked_out(void)
{
    init_test_context();
    uint8_t msg[16];
    build_ici_gas_heater_msg(0x13, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "0x13: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "0x13: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_LOCKED_OUT,        "0x13: status=HEATER_LOCKED_OUT");
    TEST_ASSERT(h->on,                                 "0x13: on=true");
    TEST_ASSERT(h->water_flow_detected,                "0x13: water flow detected");
    TEST_ASSERT(h->locked_out,                         "0x13: locked out");
    TEST_ASSERT(h->burner_state == BURNER_OFF,         "0x13: burner off");
}

void test_gas_heater_hinrg_heating(void)
{
    // Confirms the HiNRG (0x0072) pattern also dispatches to the same handler
    init_test_context();
    uint8_t msg[] = {
        0x02, 0x00, 0x72, 0xFF, 0xFF, 0x80, 0x00,
        0x12, 0x10,  // cmd, length (16)
        0x14,        // header checksum
        0x00, 0x0F, 0x00, 0x00,  // payload: status=0x0F (heating)
        0x0F,        // data checksum
        0x03
    };
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "HiNRG 0x0F: should decode");
    TEST_ASSERT(h->gas_heater_valid,                   "HiNRG 0x0F: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_HEATING,           "HiNRG 0x0F: status=HEATER_HEATING");
    TEST_ASSERT(h->burner_state == BURNER_ALIGHT,      "HiNRG 0x0F: burner alight");
}

void test_gas_heater_viron_heating(void)
{
    init_test_context();
    uint8_t msg[] = {
        0x02, 0x00, 0x71, 0xFF, 0xFF, 0x80, 0x00,
        0x12, 0x10, 0x13,
        0x00, 0x0F, 0x00, 0x00,
        0x0F,
        0x03
    };
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded, "Viron 0x0F: should decode");
    TEST_ASSERT(h->gas_heater_valid, "Viron 0x0F: gas_heater_valid");
    TEST_ASSERT(h->status == HEATER_HEATING, "Viron 0x0F: status=HEATER_HEATING");
    TEST_ASSERT(h->burner_state == BURNER_ALIGHT, "Viron 0x0F: burner alight");
}

// ======================================================
// Genus Heater Status Tests (CMD 0x12)
//
// Genus frame: 02 00 70 FF FF 80 00 12 10 12 00 SS 00 00 SS 03
// Same shape as the gas heaters but heat-pump semantics: only the
// observed status set {0x00, 0x02, 0x03, 0x07, 0x13} is accepted,
// and only heater on/off (bit 0) feeds pool state (heaters[0]).
// ======================================================

static void build_genus_heater_msg(uint8_t status_byte, uint8_t *buf)
{
    buf[0]  = 0x02; buf[1]  = 0x00; buf[2]  = 0x70;
    buf[3]  = 0xFF; buf[4]  = 0xFF; buf[5]  = 0x80;
    buf[6]  = 0x00; buf[7]  = 0x12; buf[8]  = 0x10;
    buf[9]  = 0x12; // header checksum
    buf[10] = 0x00; buf[11] = status_byte;
    buf[12] = 0x00; buf[13] = 0x00;
    buf[14] = status_byte; // data checksum = sum of payload bytes
    buf[15] = 0x03;
}

void test_genus_heater_off(void)
{
    init_test_context();
    uint8_t msg[16];
    build_genus_heater_msg(0x00, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "Genus 0x00: should decode");
    TEST_ASSERT(h->valid,                              "Genus 0x00: heater valid");
    TEST_ASSERT(!h->on,                                "Genus 0x00: on=false");
    TEST_ASSERT(!h->gas_heater_valid,                  "Genus 0x00: gas fields not claimed");
}

void test_genus_heater_off_with_flow(void)
{
    init_test_context();
    uint8_t msg[16];
    build_genus_heater_msg(0x02, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "Genus 0x02: should decode");
    TEST_ASSERT(h->valid,                              "Genus 0x02: heater valid");
    TEST_ASSERT(!h->on,                                "Genus 0x02: on=false");
}

void test_genus_heater_setpoint_reached(void)
{
    init_test_context();
    uint8_t msg[16];
    build_genus_heater_msg(0x03, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "Genus 0x03: should decode");
    TEST_ASSERT(h->valid,                              "Genus 0x03: heater valid");
    TEST_ASSERT(h->on,                                 "Genus 0x03: on=true");
}

void test_genus_heater_starting_up(void)
{
    init_test_context();
    uint8_t msg[16];
    build_genus_heater_msg(0x07, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "Genus 0x07: should decode");
    TEST_ASSERT(h->valid,                              "Genus 0x07: heater valid");
    TEST_ASSERT(h->on,                                 "Genus 0x07: on=true");
}

void test_genus_heater_heating(void)
{
    init_test_context();
    uint8_t msg[16];
    build_genus_heater_msg(0x13, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "Genus 0x13: should decode");
    TEST_ASSERT(h->valid,                              "Genus 0x13: heater valid");
    TEST_ASSERT(h->on,                                 "Genus 0x13: on=true");
    TEST_ASSERT(!h->gas_heater_valid,                  "Genus 0x13: not treated as gas lockout");
    TEST_ASSERT(!h->locked_out,                        "Genus 0x13: not locked out");
}

void test_genus_heater_undocumented_status(void)
{
    // 0x0F is a valid gas-heater value (Heating) but has never been observed
    // from the Genus — it must be flagged as undocumented, not decoded.
    init_test_context();
    uint8_t msg[16];
    build_genus_heater_msg(0x0F, msg);
    bool decoded = decode_message(msg, sizeof(msg), &test_ctx);
    const pool_heater_t *h = &test_pool_state.heaters[0];
    TEST_ASSERT(decoded,                               "Genus 0x0F: consumed (recorded as undocumented)");
    TEST_ASSERT(!h->valid,                             "Genus 0x0F: heater state not updated");
}

/**
 * Run all tests
 */
static void test_solar_mode_and_output_are_independent(void)
{
    init_test_context();
    test_ctx.enable_mqtt = true;
    uint8_t type[] = {0x6D, 2, 0xFC};
    uint8_t config[] = {0x2B, 0, 2, 0, 2, 4};
    uint8_t status[] = {0, 4, 0};
    decode_test_frame(0x50, 0x38, type, sizeof(type));
    decode_test_frame(0x50, 0x2C, config, sizeof(config));
    TEST_ASSERT(!test_pool_state.solar_active_valid,
                "Off mode and persistent config 02 do not establish activity");
    int before = unknown_buffer_record_count;
    decode_test_frame(0x62, 0x12, status, sizeof(status));
    TEST_ASSERT(unknown_buffer_record_count == before,
                "Controller Solar bit is documented");
    TEST_ASSERT(test_pool_state.channels[1].state == 0 && test_pool_state.channels[1].active,
                "Mode Off does not overwrite a still-active physical output report");
    TEST_ASSERT(!test_pool_state.heaters[0].valid,
                "Solar sample does not bypass heater configuration guards");
    TEST_ASSERT(test_pool_state.solar_status_byte == 4 && test_pool_state.solar_status_ms == mock_ticks,
                "Solar diagnostics identify the genuine controller sample");
    mock_ticks += 1000;
    decode_test_frame(0x50, 0x2C, config, sizeof(config));
    TEST_ASSERT(test_pool_state.solar_status_ms != mock_ticks,
                "Mode report does not refresh physical output sample time");
    decode_test_frame(0x62, 0x12, status, sizeof(status));
    TEST_ASSERT(test_pool_state.solar_status_ms == mock_ticks,
                "Unchanged real output report refreshes its sample time");
    uint8_t mask[] = {0x80};
    decode_test_frame(0x50, 0x0D, mask, sizeof(mask));
    TEST_ASSERT(test_pool_state.channels[1].active,
                "Ordinary channel bitmap cannot clear the dedicated output");
    status[1] = 0;
    decode_test_frame(0x62, 0x12, status, sizeof(status));
    TEST_ASSERT(!test_pool_state.channels[1].active && test_pool_state.channels[1].state == 0,
                "Physical stop clears activity while mode remains Off");
    decode_test_frame(0x50, 0x2C, config, sizeof(config));
    TEST_ASSERT(!test_pool_state.channels[1].active,
                "Persistent config 02 cannot reactivate a stopped pump");
    config[1] = 2;
    decode_test_frame(0x50, 0x2C, config, sizeof(config));
    TEST_ASSERT(test_pool_state.channels[1].state == 2 && !test_pool_state.channels[1].active,
                "On mode does not invent a running output before a real sample");
    uint8_t relay_type[] = {0x6E, 2, 0x08};
    decode_test_frame(0x50, 0x38, relay_type, sizeof(relay_type));
    status[1] = 4;
    decode_test_frame(0x62, 0x12, status, sizeof(status));
    TEST_ASSERT(!test_pool_state.channels[2].active,
                "Controller Solar bit does not affect a relay Solar channel");
    set_heater_config(HEATER1_TYPE_CONNECT_INBUILT, false);
    status[1] = 7;
    decode_test_frame(0x62, 0x12, status, sizeof(status));
    TEST_ASSERT(test_pool_state.heaters[0].on && test_pool_state.service_mode && test_pool_state.solar_active,
                "Combined heater/service/Solar bits remain independent");
    before = unknown_buffer_record_count;
    status[1] = 8;
    decode_test_frame(0x62, 0x12, status, sizeof(status));
    TEST_ASSERT(unknown_buffer_record_count > before,
                "Unmapped upper controller bits are still flagged");
}

static void test_temperature_samples_are_independent(void)
{
    init_test_context();
    test_ctx.enable_mqtt = true;
    memset(temperature_publish_calls, 0, sizeof(temperature_publish_calls));
    uint8_t temps[] = {24, 46};
    TEST_ASSERT(decode_test_frame(0x62, 0x16, temps, 2), "Two valid temperatures decoded");
    TEST_ASSERT(temperature_publish_calls[0] == 1 && temperature_publish_calls[1] == 1,
                "Both sensors published from the same frame");
    temps[1] = 0xFF;
    decode_test_frame(0x62, 0x16, temps, 2);
    TEST_ASSERT(temperature_publish_calls[0] == 2 && temperature_publish_calls[1] == 1,
                "Invalid roof sample does not republish its cached reading");
    temps[0] = 0xFF; temps[1] = 47;
    decode_test_frame(0x62, 0x16, temps, 2);
    TEST_ASSERT(temperature_publish_calls[0] == 2 && temperature_publish_calls[1] == 2,
                "Valid roof sample publishes even when water sample is invalid");
    temps[1] = 0xFF;
    decode_test_frame(0x62, 0x16, temps, 2);
    TEST_ASSERT(temperature_publish_calls[0] == 2 && temperature_publish_calls[1] == 2,
                "Two invalid samples do not create history points");
    temps[0] = 24;
    decode_test_frame(0x62, 0x16, temps, 1);
    TEST_ASSERT(temperature_publish_calls[0] == 3 && temperature_publish_calls[1] == 2,
                "Missing roof sample does not republish its cached reading");
    decode_test_frame(0x62, 0x31, temps, 1);
    TEST_ASSERT(temperature_publish_calls[0] == 3,
                "Alternate CMD 0x31 remains log-only and does not duplicate history");
}

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
    test_heater2_setpoint_registers();
    test_heater_device_setpoint_broadcast();
    test_viron_heater_setpoint_broadcast();
    test_heater_device_setpoint_unplumbed_circuit();
    test_heater_device_setpoint_config_unknown();
    test_heater_device_setpoint_heater2_removed();
    test_heater_device_setpoint_heater2_configured();
    test_heater_config_registers();
    test_heater_configured_helper();
    test_heater_config_change_publishes();
    test_decode_temp_reading();
    test_temperature_samples_are_independent();
    test_decode_heater_on();
    test_decode_heater_off();
    test_decode_heater_bit_ignored_until_type_known();
    test_decode_heater_bit_ignored_for_genus();

    // Channel status tests
    printf("\n--- Channel Status Tests ---\n");
    test_decode_channel_status_all_off();
    test_channel_status_resets_pump_speed_when_filter_off();
    test_channel_status_does_not_fabricate_pump_speed_without_telemetry();
    test_decode_connect10_dedicated_solar_channel();
    test_dedicated_solar_uses_solar_status_mode_and_activity();
    test_dedicated_solar_state_register_does_not_override();
    test_dedicated_solar_status_before_type_register();
    test_dedicated_solar_undocumented_mode_keeps_active();
    test_dedicated_solar_undocumented_raw_state_flagged();
    test_solar_mode_and_output_are_independent();
    test_decode_channel_status_lights_active();
    test_decode_channel_status_multispeed_pump();
    test_channel_status_teaches_filter_pump_type();
    test_channel_state_names_cover_every_state();
    test_decode_channel_toggle_gateway();
    test_decode_channel_toggle_controller();
    test_decode_register_write_gateway();
    test_decode_chlor_unknown_countdown();
    test_decode_chlor_unknown_value();
    test_decode_register_write_chlorinator();
    test_decode_register_write_light_color();

    // Chlorinator tests
    printf("\n--- Chlorinator Tests ---\n");
    test_decode_chlor_ph_setpoint();
    test_decode_chlor_orp_setpoint();
    test_decode_chlor_set_pump_mode();
    test_decode_chlor_set_pump_mode_bad_byte0();
    test_decode_chlor_set_pump_mode_bad_mode();

    // Pump tests
    printf("\n--- Pump Tests ---\n");
    test_decode_pump_speed_1125();
    test_decode_pump_speed_1350();
    test_decode_pump_speed_1500();
    test_decode_pump_speed_zero();
    test_decode_pump_activity();

    // Gas heater status tests (CMD 0x12)
    printf("\n--- Gas Heater Status Tests ---\n");
    test_gas_heater_ici_off();
    test_gas_heater_ici_no_flow();
    test_gas_heater_ici_off_with_flow();
    test_gas_heater_ici_setpoint_reached();
    test_gas_heater_ici_igniting();
    test_gas_heater_ici_heating();
    test_gas_heater_ici_cooldown();
    test_gas_heater_ici_locked_out();
    test_gas_heater_hinrg_heating();
    test_gas_heater_viron_heating();

    // Genus heater status tests (CMD 0x12)
    printf("\n--- Genus Heater Status Tests ---\n");
    test_genus_heater_off();
    test_genus_heater_off_with_flow();
    test_genus_heater_setpoint_reached();
    test_genus_heater_starting_up();
    test_genus_heater_heating();
    test_genus_heater_undocumented_status();

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
