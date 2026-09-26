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
#include "../main/pool_state.h"

// ======================================================
// Bus spy
//
// mqtt_commands.c sends frames via bus_send_bytes() (see main/bus.h). We
// intercept that call here so each test can assert on the exact bytes the
// command builder produced.
// ======================================================

static uint8_t  s_uart_buf[64];
static int      s_uart_len   = 0;
static int      s_uart_calls = 0;

int bus_send_bytes(const uint8_t *data, size_t len)
{
    s_uart_len = (int)len;
    s_uart_calls++;
    if (len <= sizeof(s_uart_buf)) {
        memcpy(s_uart_buf, data, len);
    }
    return (int)len;
}

// ======================================================
// Register read-back spy
//
// After writing a setpoint, mqtt_commands.c queues a CMD 0x39 read-back of
// the register it just wrote (see main/register_requester.h). The real
// implementation needs FreeRTOS, so record the request here instead.
// ======================================================

static uint8_t s_read_back_reg   = 0;
static uint8_t s_read_back_slot  = 0;
static int     s_read_back_calls = 0;

void register_requester_read_back(uint8_t reg_id, uint8_t slot)
{
    s_read_back_reg  = reg_id;
    s_read_back_slot = slot;
    s_read_back_calls++;
}

// ======================================================
// Channel power spies
//
// handle_channel_power_command() persists the configured wattage via
// channel_power_set_configured() (main/channel_power.h) and republishes the
// channel via mqtt_publish_channel() (main/mqtt_publish.h). Both pull in
// NVS/MQTT plumbing the host build doesn't have, so stub them here instead
// of linking the real modules.
// ======================================================

static uint8_t  s_power_channel_id = 0;
static uint16_t s_power_watts      = 0;
static int      s_power_set_calls  = 0;

esp_err_t channel_power_set_configured(uint8_t channel_id, uint16_t watts)
{
    s_power_channel_id = channel_id;
    s_power_watts = watts;
    s_power_set_calls++;
    return ESP_OK;
}

static uint8_t s_published_channel_id = 0;
static int     s_publish_channel_calls = 0;

void mqtt_publish_channel(const pool_state_t *current_state, uint8_t channel_id)
{
    (void)current_state;
    s_published_channel_id = channel_id;
    s_publish_channel_calls++;
}

// handle_system_power_command() takes the same shape for the system baseline:
// persist via channel_power_set_system(), then republish.

static uint16_t s_system_watts            = 0;
static int      s_system_set_calls        = 0;
static int      s_publish_system_calls    = 0;

esp_err_t channel_power_set_system(uint16_t watts)
{
    s_system_watts = watts;
    s_system_set_calls++;
    return ESP_OK;
}

void mqtt_publish_system_power(void)
{
    s_publish_system_calls++;
}

static void uart_spy_reset(void)
{
    memset(s_uart_buf, 0, sizeof(s_uart_buf));
    s_uart_len   = 0;
    s_uart_calls = 0;

    s_read_back_reg   = 0;
    s_read_back_slot  = 0;
    s_read_back_calls = 0;

    s_power_channel_id = 0;
    s_power_watts       = 0;
    s_power_set_calls   = 0;

    s_published_channel_id  = 0;
    s_publish_channel_calls = 0;

    s_system_watts         = 0;
    s_system_set_calls     = 0;
    s_publish_system_calls = 0;
}

// ======================================================
// FreeRTOS / pool_state globals
//
// mqtt_commands.c references s_pool_state and s_pool_state_mutex (used by the
// favourite-name lookup) and takes the mutex via xSemaphoreTake. The
// favourite tests in this file don't exercise that path, but the symbols
// must exist at link time. Leaving the mutex NULL makes mqtt_commands.c skip
// the favourites lookup safely.
// ======================================================

pool_state_t       s_pool_state;
SemaphoreHandle_t  s_pool_state_mutex = NULL;

uint32_t xTaskGetTickCount(void) { return 1000; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)s; (void)t;
    return pdTRUE;
}

void xSemaphoreGive(SemaphoreHandle_t s) { (void)s; }

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

void test_heater_1_on(void)
{
    send_cmd("heater/1/set", "ON");

    // Heater 2 on/off = register 0xE9. Checksum = (0xE9 + 0x00 + 0x01) & 0xFF = 0xEA
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xE9, 0x00, 0x01,
        0xEA,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "heater/1/set ON: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "heater/1/set ON: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "heater/1/set ON: correct bytes");
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
// Channel power config tests
//
// "channel/N/set" and "channel/N/power/set" share a topic prefix and are
// separated by an exact suffix-length match in mqtt_handle_command. These
// pin that routing: the right handler runs, the wrong one doesn't, and a
// near-miss suffix falls through to neither.
// ======================================================

void test_channel_power_set(void)
{
    send_cmd("channel/3/power/set", "750");

    TEST_ASSERT(s_power_set_calls == 1, "channel/3/power/set: persists once");
    TEST_ASSERT(s_power_channel_id == 3, "channel/3/power/set: correct channel");
    TEST_ASSERT(s_power_watts == 750, "channel/3/power/set: correct wattage");
    TEST_ASSERT(s_uart_calls == 0, "channel/3/power/set: local config, no bus write");
}

void test_channel_power_republishes(void)
{
    // The republish is guarded on s_pool_state_mutex, which the rest of this
    // file leaves NULL (see the globals section). Point it at a dummy for the
    // duration: xSemaphoreTake is stubbed to always succeed, and
    // mqtt_publish_channel is a spy, so nothing real is touched.
    static int dummy_mutex;
    s_pool_state_mutex = &dummy_mutex;

    send_cmd("channel/3/power/set", "750");

    TEST_ASSERT(s_publish_channel_calls == 1, "channel/3/power/set: republishes channel");
    TEST_ASSERT(s_published_channel_id == 3, "channel/3/power/set: republishes correct channel");

    s_pool_state_mutex = NULL;
}

void test_channel_power_rejected_does_not_republish(void)
{
    static int dummy_mutex;
    s_pool_state_mutex = &dummy_mutex;

    send_cmd("channel/3/power/set", "abc");

    TEST_ASSERT(s_publish_channel_calls == 0,
                "channel/3/power/set abc: no republish even with a live mutex");

    s_pool_state_mutex = NULL;
}

void test_channel_power_zero_clears(void)
{
    send_cmd("channel/3/power/set", "0");

    // 0 is a meaningful value (clears the manual estimate), not a rejection.
    TEST_ASSERT(s_power_set_calls == 1, "channel/3/power/set 0: persists");
    TEST_ASSERT(s_power_watts == 0, "channel/3/power/set 0: wattage cleared");
}

void test_channel_power_max(void)
{
    send_cmd("channel/8/power/set", "65535");

    TEST_ASSERT(s_power_set_calls == 1, "channel/8/power/set 65535: accepted at uint16 max");
    TEST_ASSERT(s_power_watts == 65535, "channel/8/power/set 65535: correct wattage");
}

void test_channel_toggle_is_not_power(void)
{
    send_cmd("channel/3/set", "TOGGLE");

    TEST_ASSERT(s_uart_calls == 1, "channel/3/set: routed to toggle");
    TEST_ASSERT(s_power_set_calls == 0, "channel/3/set: does not reach the power handler");
}

void test_channel_power_out_of_range(void)
{
    send_cmd("channel/9/power/set", "750");

    TEST_ASSERT(s_power_set_calls == 0, "channel/9/power/set: rejected (> MAX_CHANNELS)");
}

void test_channel_power_suffix_near_miss(void)
{
    send_cmd("channel/3/power/setx", "750");

    TEST_ASSERT(s_power_set_calls == 0, "channel/3/power/setx: unrecognised suffix rejected");
    TEST_ASSERT(s_uart_calls == 0, "channel/3/power/setx: no bus write either");
}

void test_channel_power_non_numeric(void)
{
    send_cmd("channel/3/power/set", "abc");

    TEST_ASSERT(s_power_set_calls == 0, "channel/3/power/set abc: rejected");
}

void test_channel_power_trailing_garbage(void)
{
    send_cmd("channel/3/power/set", "750W");

    // strtol would stop at 'W' and report 750 — the handler requires the
    // whole payload to be consumed.
    TEST_ASSERT(s_power_set_calls == 0, "channel/3/power/set 750W: partial parse rejected");
}

void test_channel_power_overflow(void)
{
    send_cmd("channel/3/power/set", "70000");

    TEST_ASSERT(s_power_set_calls == 0, "channel/3/power/set 70000: rejected (> uint16)");
}

void test_channel_power_negative(void)
{
    send_cmd("channel/3/power/set", "-5");

    TEST_ASSERT(s_power_set_calls == 0, "channel/3/power/set -5: rejected");
}

void test_channel_power_empty_payload(void)
{
    send_cmd("channel/3/power/set", "");

    TEST_ASSERT(s_power_set_calls == 0, "channel/3/power/set empty: rejected");
}

// ======================================================
// System baseline power tests
// ======================================================

void test_system_power_set(void)
{
    send_cmd("system/power/set", "45");

    TEST_ASSERT(s_system_set_calls == 1, "system/power/set: persists once");
    TEST_ASSERT(s_system_watts == 45, "system/power/set: correct wattage");
    TEST_ASSERT(s_publish_system_calls == 1, "system/power/set: republishes");
    TEST_ASSERT(s_power_set_calls == 0, "system/power/set: not routed to a channel");
    TEST_ASSERT(s_uart_calls == 0, "system/power/set: local config, no bus write");
}

void test_system_power_zero_clears(void)
{
    send_cmd("system/power/set", "0");

    TEST_ASSERT(s_system_set_calls == 1, "system/power/set 0: persists");
    TEST_ASSERT(s_system_watts == 0, "system/power/set 0: baseline cleared");
}

void test_system_power_max(void)
{
    send_cmd("system/power/set", "65535");

    TEST_ASSERT(s_system_set_calls == 1, "system/power/set 65535: accepted at uint16 max");
    TEST_ASSERT(s_system_watts == 65535, "system/power/set 65535: correct wattage");
}

void test_system_power_non_numeric(void)
{
    send_cmd("system/power/set", "abc");

    TEST_ASSERT(s_system_set_calls == 0, "system/power/set abc: rejected");
    TEST_ASSERT(s_publish_system_calls == 0, "system/power/set abc: nothing republished");
}

void test_system_power_overflow(void)
{
    send_cmd("system/power/set", "70000");

    TEST_ASSERT(s_system_set_calls == 0, "system/power/set 70000: rejected (> uint16)");
}

// ======================================================
// Mode tests
// ======================================================

void test_mode_pool(void)
{
    send_cmd("mode/set", "Pool");

    // Expected: 02 00 50 FF FF 80 00 15 0D F2 01 01 03
    // mode_value=0x00 (Pool), checksum=0x00
    uint8_t expected[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x15, 0x0D, 0xF2,
        0x01, 0x01,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "mode/set Pool: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "mode/set Pool: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "mode/set Pool: correct bytes");
}

void test_mode_spa(void)
{
    send_cmd("mode/set", "Spa");
    // Expected: 02 00 50 FF FF 80 00 15 0D F2 00 00 03
    // mode_value=0x01 (Spa), checksum=0x01
    uint8_t expected[] = {
        0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00,
        0x15, 0x0D, 0xF2,
        0x00, 0x00,
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
    // Heater 1 pool setpoint uses the system pool setpoint command (CMD 0x19).
    send_cmd("heater/0/pool_setpoint/set", "30");

    // target=0x01 (Pool), temp=30=0x1E, checksum=(0x01+0x1E+0x1E)&0xFF=0x3D
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x19, 0x0F, 0x98,
        0x01, 0x1E, 0x1E,
        0x3D,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "heater/0/pool_setpoint/set 30: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "heater/0/pool_setpoint/set 30: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "heater/0/pool_setpoint/set 30: correct bytes");
    TEST_ASSERT(s_read_back_calls == 1 && s_read_back_reg == 0xE7 && s_read_back_slot == 0x00,
                "heater/0/pool_setpoint/set 30: reads register 0xE7 back");
}

void test_temperature_spa(void)
{
    // Heater 1 spa setpoint uses the system spa setpoint command (CMD 0x19).
    send_cmd("heater/0/spa_setpoint/set", "37");

    // target=0x02 (Spa), temp=37=0x25, checksum=(0x02+0x25+0x25)&0xFF=0x4C
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x19, 0x0F, 0x98,
        0x02, 0x25, 0x25,
        0x4C,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "heater/0/spa_setpoint/set 37: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "heater/0/spa_setpoint/set 37: correct bytes");
    TEST_ASSERT(s_read_back_calls == 1 && s_read_back_reg == 0xE8 && s_read_back_slot == 0x00,
                "heater/0/spa_setpoint/set 37: reads register 0xE8 back");
}

void test_heater2_pool_setpoint(void)
{
    // Heater 2 pool setpoint = gateway register write (CMD 0x3A) to 0xEA.
    send_cmd("heater/1/pool_setpoint/set", "27");

    // reg=0xEA, slot=0x00, temp=27=0x1B, checksum=(0xEA+0x00+0x1B)&0xFF=0x05
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xEA, 0x00, 0x1B,
        0x05,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "heater/1/pool_setpoint/set 27: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "heater/1/pool_setpoint/set 27: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "heater/1/pool_setpoint/set 27: correct bytes");
    TEST_ASSERT(s_read_back_calls == 1 && s_read_back_reg == 0xEA && s_read_back_slot == 0x00,
                "heater/1/pool_setpoint/set 27: reads register 0xEA back");
}

void test_heater2_spa_setpoint(void)
{
    // Heater 2 spa setpoint = gateway register write (CMD 0x3A) to 0xEB.
    send_cmd("heater/1/spa_setpoint/set", "24");

    // reg=0xEB, slot=0x00, temp=24=0x18, checksum=(0xEB+0x00+0x18)&0xFF=0x03
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xEB, 0x00, 0x18,
        0x03,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "heater/1/spa_setpoint/set 24: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "heater/1/spa_setpoint/set 24: correct bytes");
    TEST_ASSERT(s_read_back_calls == 1 && s_read_back_reg == 0xEB && s_read_back_slot == 0x00,
                "heater/1/spa_setpoint/set 24: reads register 0xEB back");
}

void test_temperature_out_of_range(void)
{
    send_cmd("heater/0/pool_setpoint/set", "100");
    TEST_ASSERT(s_uart_calls == 0, "heater/0/pool_setpoint/set 100: no UART write (out of range)");
}

void test_temperature_invalid(void)
{
    send_cmd("heater/0/pool_setpoint/set", "warm");
    TEST_ASSERT(s_uart_calls == 0, "heater/0/pool_setpoint/set 'warm': no UART write (non-numeric)");
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

void test_light_1_color_blue(void)
{
    send_cmd("light/1/color/set", "Blue");

    // reg_id=0xD0, color=0x05 (Blue), checksum=(0xD0+0x01+0x05)&0xFF=0xD6
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xD0, 0x01, 0x05,
        0xD6,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "light/1/color/set Blue: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "light/1/color/set Blue: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "light/1/color/set Blue: correct bytes");
}

void test_light_2_color_magenta(void)
{
    send_cmd("light/2/color/set", "Magenta");

    // reg_id=0xD1, color=0x0D (Magenta), checksum=(0xD1+0x01+0x0D)&0xFF=0xDF
    uint8_t expected[] = {
        0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00,
        0x3A, 0x0F, 0xB9,
        0xD1, 0x01, 0x0D,
        0xDF,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "light/2/color/set Magenta: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0, "light/2/color/set Magenta: correct bytes");
}

void test_light_color_unknown(void)
{
    send_cmd("light/1/color/set", "Chartreuse");
    TEST_ASSERT(s_uart_calls == 0, "light/1/color/set Chartreuse: no UART write (unknown color)");
}


// ======================================================
// Channel pump-mode tests (CMD 0x0F direct state set)
// ======================================================

// The frame claims the chlorinator address most recently seen on the bus; the
// default until one has been heard is 0x0084, which is confirmed working even
// on a bus where no such device exists.
// The pump-mode handler reads the latched source under the pool-state mutex,
// which the rest of this file deliberately leaves NULL. Hand it a non-NULL
// token for these tests (xSemaphoreTake above succeeds regardless) and put it
// back afterwards so the other groups keep their skip-the-lookup behaviour.
static void pump_mode_setup(uint8_t src_lo)
{
    s_pool_state_mutex = (SemaphoreHandle_t)1;
    s_pool_state.chlor_src_hi = 0x00;
    s_pool_state.chlor_src_lo = src_lo;
}

static void pump_mode_teardown(void)
{
    s_pool_state_mutex = NULL;
}

void test_channel_mode_high_speed(void)
{
    pump_mode_setup(0x84);
    send_cmd("channel/1/mode/set", "High");

    // Header checksum = sum(bytes 0-8) = 02+00+84+00+50+80+00+0F+0E = 0x73
    // Data checksum = channel (1) + state (5) = 0x06
    uint8_t expected[] = {
        0x02, 0x00, 0x84, 0x00, 0x50, 0x80, 0x00,
        0x0F, 0x0E, 0x73,
        0x01, 0x05,
        0x06,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "channel/1/mode/set High: exactly one UART write");
    TEST_ASSERT(s_uart_len == sizeof(expected), "channel/1/mode/set High: correct length");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0,
                "channel/1/mode/set High: correct bytes");
}

// A latched 0x0081 source changes the header checksum too, so this also covers
// the checksum being computed rather than copied from a fixed pattern.
void test_channel_mode_uses_latched_chlorinator(void)
{
    pump_mode_setup(0x81);
    send_cmd("channel/1/mode/set", "Off");

    // Header checksum = 02+00+81+00+50+80+00+0F+0E = 0x70
    uint8_t expected[] = {
        0x02, 0x00, 0x81, 0x00, 0x50, 0x80, 0x00,
        0x0F, 0x0E, 0x70,
        0x01, 0x00,
        0x01,
        0x03
    };
    TEST_ASSERT(s_uart_calls == 1, "channel/1/mode/set Off: exactly one UART write");
    TEST_ASSERT(memcmp(s_uart_buf, expected, sizeof(expected)) == 0,
                "channel/1/mode/set Off: uses the latched 0x0081 source");
}

// Unlike the toggle command's 0-based index, byte 10 here is 1-based.
void test_channel_mode_channel_is_one_based(void)
{
    pump_mode_setup(0x84);
    send_cmd("channel/2/mode/set", "Auto");

    TEST_ASSERT(s_uart_calls == 1, "channel/2/mode/set Auto: exactly one UART write");
    TEST_ASSERT(s_uart_buf[10] == 0x02, "channel/2/mode/set Auto: channel byte is 1-based");
    TEST_ASSERT(s_uart_buf[11] == 0x01, "channel/2/mode/set Auto: state byte is Auto");
    TEST_ASSERT(s_uart_buf[12] == 0x03, "channel/2/mode/set Auto: data checksum is channel+state");
}

void test_channel_mode_unknown_payload(void)
{
    send_cmd("channel/1/mode/set", "Turbo");
    TEST_ASSERT(s_uart_calls == 0, "channel/1/mode/set Turbo: nothing sent for an unknown mode");
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
    test_heater_1_on();
    test_heater_out_of_range();
    test_heater_malformed_topic();
    test_heater_invalid_payload();

    printf("\n--- Channel Tests ---\n");
    test_channel_1_toggle();
    test_channel_5_toggle();
    test_channel_out_of_range();

    printf("\n--- Channel power config ---\n");
    test_channel_power_set();
    test_channel_power_republishes();
    test_channel_power_rejected_does_not_republish();
    test_channel_power_zero_clears();
    test_channel_power_max();
    test_channel_toggle_is_not_power();
    test_channel_power_out_of_range();
    test_channel_power_suffix_near_miss();
    test_channel_power_non_numeric();
    test_channel_power_trailing_garbage();
    test_channel_power_overflow();
    test_channel_power_negative();
    test_channel_power_empty_payload();

    printf("\n--- System baseline power ---\n");
    test_system_power_set();
    test_system_power_zero_clears();
    test_system_power_max();
    test_system_power_non_numeric();
    test_system_power_overflow();

    printf("\n--- Mode Tests ---\n");
    test_mode_pool();
    test_mode_spa();
    test_mode_invalid();

    printf("\n--- Temperature Tests ---\n");
    test_temperature_pool();
    test_temperature_spa();
    test_heater2_pool_setpoint();
    test_heater2_spa_setpoint();
    test_temperature_out_of_range();
    test_temperature_invalid();

    printf("\n--- Light Tests ---\n");
    test_light_1_on();
    test_light_2_off();
    test_light_invalid_payload();
    test_light_1_color_blue();
    test_light_2_color_magenta();
    test_light_color_unknown();

    printf("\n--- Channel Pump Mode Tests ---\n");
    test_channel_mode_high_speed();
    test_channel_mode_uses_latched_chlorinator();
    test_channel_mode_channel_is_one_based();
    test_channel_mode_unknown_payload();
    pump_mode_teardown();

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
