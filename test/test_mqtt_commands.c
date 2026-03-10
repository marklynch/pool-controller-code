/**
 * Unit tests for mqtt_commands module
 *
 * To run:
 *   cd test && gcc -I. -I.. -o test_commands test_mqtt_commands.c ../main/mqtt_commands.c && ./test_commands
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../main/mqtt_commands.h"
#include "driver/uart.h"

// ======================================================
// UART spy
// ======================================================

static uint8_t  s_uart_buf[64];
static int      s_uart_len   = 0;
static int      s_uart_calls = 0;

int uart_write_bytes(uart_port_t uart_num, const char *src, size_t size)
{
    (void)uart_num;
    s_uart_len = (int)size;
    s_uart_calls++;
    if (size <= sizeof(s_uart_buf)) {
        memcpy(s_uart_buf, src, size);
    }
    return (int)size;
}

static void uart_spy_reset(void)
{
    memset(s_uart_buf, 0, sizeof(s_uart_buf));
    s_uart_len   = 0;
    s_uart_calls = 0;
}

// ======================================================
// mqtt_get_device_id stub — fixed ID used in all topics
// ======================================================

#define TEST_DEVICE_ID "testdevice"

void mqtt_get_device_id(char *device_id, size_t max_len)
{
    strncpy(device_id, TEST_DEVICE_ID, max_len - 1);
    device_id[max_len - 1] = '\0';
}

// ======================================================
// Test helpers
// ======================================================

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

// Send a command with the standard test device topic prefix
static void send_cmd(const char *suffix, const char *payload)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "pool/" TEST_DEVICE_ID "/%s", suffix);
    uart_spy_reset();
    mqtt_handle_command(topic, (int)strlen(topic), payload, (int)strlen(payload));
}

// ======================================================
// Heater tests
// ======================================================

void test_heater_0_on(void)
{
    send_cmd("heater/0/set", "ON");

    // Expected: 02 00 F0 FF FF 80 00 3A 0F B9 E6 00 01 E7 03
    // Checksum = (0xE6 + 0x00 + 0x01) & 0xFF = 0xE7
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xE6, 0x00, 0x01,
        0xE7,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "heater/0/set ON: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "heater/0/set ON: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "heater/0/set ON: correct bytes");
}

void test_heater_0_off(void)
{
    send_cmd("heater/0/set", "OFF");

    // Expected: 02 00 F0 FF FF 80 00 3A 0F B9 E6 00 00 E6 03
    // Checksum = (0xE6 + 0x00 + 0x00) & 0xFF = 0xE6
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xE6, 0x00, 0x00,
        0xE6,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "heater/0/set OFF: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "heater/0/set OFF: correct bytes");
}

void test_heater_1_unsupported(void)
{
    send_cmd("heater/1/set", "ON");
    TEST_ASSERT(s_uart_calls == 0, "heater/1/set: no UART write (not yet supported)");
}

void test_heater_out_of_range(void)
{
    send_cmd("heater/99/set", "ON");
    TEST_ASSERT(s_uart_calls == 0, "heater/99/set: no UART write (out of range)");
}

void test_heater_malformed_topic(void)
{
    send_cmd("heater/abc/set", "ON");
    TEST_ASSERT(s_uart_calls == 0, "heater/abc/set: no UART write (malformed index)");
}

void test_heater_invalid_payload(void)
{
    send_cmd("heater/0/set", "MAYBE");
    TEST_ASSERT(s_uart_calls == 0, "heater/0/set MAYBE: no UART write (invalid payload)");
}

// ======================================================
// Channel tests
// ======================================================

void test_channel_1_toggle(void)
{
    send_cmd("channel/1/set", "TOGGLE");

    // Expected: 02 00 F0 FF FF 80 00 10 0D 8D 00 00 03
    // channel_idx = 0, checksum = 0x00
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x10, 0x0D, 0x8D,
        0x00, 0x00,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "channel/1/set: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "channel/1/set: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "channel/1/set: correct bytes");
}

void test_channel_5_toggle(void)
{
    send_cmd("channel/5/set", "TOGGLE");

    // channel_idx = 4, checksum = 0x04
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x10, 0x0D, 0x8D,
        0x04, 0x04,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "channel/5/set: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "channel/5/set: correct bytes");
}

void test_channel_out_of_range(void)
{
    send_cmd("channel/99/set", "TOGGLE");
    TEST_ASSERT(s_uart_calls == 0, "channel/99/set: no UART write (out of range)");
}

// ======================================================
// Mode tests
// ======================================================

void test_mode_pool(void)
{
    send_cmd("mode/set", "Pool");

    // Expected: 02 00 F0 00 50 80 00 2A 0D F9 00 00 03
    // mode_value=0x00 (Pool), checksum=0x00
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0x00, 0x50, 0x80, 0x00,
        0x2A, 0x0D, 0xF9,
        0x00, 0x00,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "mode/set Pool: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "mode/set Pool: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "mode/set Pool: correct bytes");
}

void test_mode_spa(void)
{
    send_cmd("mode/set", "Spa");

    // mode_value=0x01 (Spa), checksum=0x01
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0x00, 0x50, 0x80, 0x00,
        0x2A, 0x0D, 0xF9,
        0x01, 0x01,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "mode/set Spa: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "mode/set Spa: correct bytes");
}

void test_mode_invalid(void)
{
    send_cmd("mode/set", "Jacuzzi");
    TEST_ASSERT(s_uart_calls == 0, "mode/set invalid: no UART write");
}

// ======================================================
// Temperature tests
// ======================================================

void test_temperature_pool(void)
{
    send_cmd("temperature/pool/set", "30");

    // target=0x01 (Pool), temp=30=0x1E, checksum=(0x01+0x1E+0x1E)&0xFF=0x3D
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x19, 0x0F, 0x98,
        0x01, 0x1E, 0x1E,
        0x3D,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "temperature/pool/set 30: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "temperature/pool/set 30: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "temperature/pool/set 30: correct bytes");
}

void test_temperature_spa(void)
{
    send_cmd("temperature/spa/set", "37");

    // target=0x02 (Spa), temp=37=0x25, checksum=(0x02+0x25+0x25)&0xFF=0x4C
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x19, 0x0F, 0x98,
        0x02, 0x25, 0x25,
        0x4C,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "temperature/spa/set 37: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "temperature/spa/set 37: correct bytes");
}

void test_temperature_out_of_range(void)
{
    send_cmd("temperature/pool/set", "100");
    TEST_ASSERT(s_uart_calls == 0, "temperature/pool/set 100: no UART write (out of range)");
}

void test_temperature_invalid(void)
{
    send_cmd("temperature/pool/set", "warm");
    TEST_ASSERT(s_uart_calls == 0, "temperature/pool/set 'warm': no UART write (non-numeric)");
}

// ======================================================
// Light tests
// ======================================================

void test_light_1_on(void)
{
    send_cmd("light/1/set", "ON");

    // reg_id=0xC0, state=0x02, checksum=(0xC0+0x01+0x02)&0xFF=0xC3
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xC0, 0x01, 0x02,
        0xC3,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "light/1/set ON: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "light/1/set ON: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "light/1/set ON: correct bytes");
}

void test_light_2_off(void)
{
    send_cmd("light/2/set", "OFF");

    // reg_id=0xC1, state=0x00, checksum=(0xC1+0x01+0x00)&0xFF=0xC2
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xC1, 0x01, 0x00,
        0xC2,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "light/2/set OFF: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "light/2/set OFF: correct bytes");
}

void test_light_invalid_payload(void)
{
    send_cmd("light/1/set", "BLINK");
    TEST_ASSERT(s_uart_calls == 0, "light/1/set BLINK: no UART write (invalid payload)");
}

// ======================================================
// Valve tests
// ======================================================

void test_valve_1_on(void)
{
    send_cmd("valve/1/set", "On");

    // valve_idx=0, state=0x02 (On), checksum=(0+2)&0xFF=0x02
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x28, 0x0E, 0xA6,
        0x00, 0x02,
        0x02,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "valve/1/set On: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "valve/1/set On: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "valve/1/set On: correct bytes");
}

void test_valve_1_auto(void)
{
    send_cmd("valve/1/set", "Auto");

    // valve_idx=0, state=0x01 (Auto), checksum=(0+1)&0xFF=0x01
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x28, 0x0E, 0xA6,
        0x00, 0x01,
        0x01,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "valve/1/set Auto: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "valve/1/set Auto: correct bytes");
}

void test_valve_invalid_payload(void)
{
    send_cmd("valve/1/set", "Toggle");
    TEST_ASSERT(s_uart_calls == 0, "valve/1/set Toggle: no UART write (invalid payload)");
}

// ======================================================
// Wrong device ID
// ======================================================

void test_wrong_device_id(void)
{
    const char *topic = "pool/otherdevice/heater/0/set";
    uart_spy_reset();
    mqtt_handle_command(topic, (int)strlen(topic), "ON", 2);
    TEST_ASSERT(s_uart_calls == 0, "wrong device ID: no UART write");
}

// ======================================================
// Unknown topic
// ======================================================

void test_unknown_topic(void)
{
    send_cmd("sprinkler/1/set", "ON");
    TEST_ASSERT(s_uart_calls == 0, "unknown topic: no UART write");
}

// ======================================================
// Main
// ======================================================

int main(void)
{
    printf("\n");
    printf("======================================\n");
    printf("  MQTT Commands Unit Tests\n");
    printf("======================================\n\n");

    printf("--- Heater Tests ---\n");
    test_heater_0_on();
    test_heater_0_off();
    test_heater_1_unsupported();
    test_heater_out_of_range();
    test_heater_malformed_topic();
    test_heater_invalid_payload();

    printf("\n--- Channel Tests ---\n");
    test_channel_1_toggle();
    test_channel_5_toggle();
    test_channel_out_of_range();

    printf("\n--- Mode Tests ---\n");
    test_mode_pool();
    test_mode_spa();
    test_mode_invalid();

    printf("\n--- Temperature Tests ---\n");
    test_temperature_pool();
    test_temperature_spa();
    test_temperature_out_of_range();
    test_temperature_invalid();

    printf("\n--- Light Tests ---\n");
    test_light_1_on();
    test_light_2_off();
    test_light_invalid_payload();

    printf("\n--- Valve Tests ---\n");
    test_valve_1_on();
    test_valve_1_auto();
    test_valve_invalid_payload();

    printf("\n--- Routing Tests ---\n");
    test_wrong_device_id();
    test_unknown_topic();

    printf("\n======================================\n");
    printf("  Test Summary\n");
    printf("======================================\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("======================================\n\n");

    return (tests_failed == 0) ? 0 : 1;
}
