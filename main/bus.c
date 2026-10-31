#include "bus.h"
#include "config.h"
#include "led_helper.h"
#include "framing.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BUS";

// ======================================================
// RX stash and TX echo tracking
// ======================================================
//
// Every byte taken from the UART driver goes through rx_pull_locked(), which
// appends it to s_rx_stash and checks it against the echo we expect while a
// transmit is in flight. bus_read() hands bytes out of the stash, so the
// reader still sees every byte (our echo included) in order. The transmit
// path can also pull, so confirming an echo doesn't depend on the reader
// task, which may itself be the one transmitting (TCP client commands).
//
// s_rx_pulled counts every byte ever pulled, giving each byte a stream offset.
// When a transmit is armed, the echo is expected to start at the offset of
// the next byte not yet received (pulled + still buffered in the driver).

#define BUS_RX_STASH_SIZE   512
#define BUS_IDLE_POLL_US    250    // Sampling interval while timing the idle window
#define BUS_ECHO_FAST_US    5000   // Poll for the echo without sleeping for this long after TX

typedef enum {
    ECHO_IDLE,
    ECHO_ARMED,
    ECHO_MATCHED,
    ECHO_MISMATCH,
} echo_state_t;

static SemaphoreHandle_t s_rx_lock;  // Guards the stash, s_rx_pulled and echo state
static SemaphoreHandle_t s_tx_lock;  // One transmitter (and one armed echo) at a time

static uint8_t  s_rx_stash[BUS_RX_STASH_SIZE];
static size_t   s_rx_stash_len;
static uint32_t s_rx_pulled;

static echo_state_t s_echo_state = ECHO_IDLE;
static uint8_t  s_echo_expected[BUS_MESSAGE_MAX_SIZE];
static size_t   s_echo_len;
static uint32_t s_echo_start;
static size_t   s_echo_bad_idx;
static uint8_t  s_echo_bad_byte;

// Compare newly pulled bytes (the first at stream offset first_offset) with
// the armed echo. Caller holds s_rx_lock.
static void echo_observe_locked(const uint8_t *data, size_t n, uint32_t first_offset)
{
    if (s_echo_state != ECHO_ARMED) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        int32_t idx = (int32_t)(first_offset + i - s_echo_start);
        if (idx < 0) {
            continue;  // received before our transmit started
        }
        if (data[i] != s_echo_expected[idx]) {
            s_echo_state = ECHO_MISMATCH;
            s_echo_bad_idx = idx;
            s_echo_bad_byte = data[i];
            return;
        }
        if ((size_t)idx == s_echo_len - 1) {
            s_echo_state = ECHO_MATCHED;
            return;
        }
    }
}

// Move whatever the UART driver has buffered into the stash. Caller holds
// s_rx_lock.
static void rx_pull_locked(void)
{
    size_t space = sizeof(s_rx_stash) - s_rx_stash_len;
    if (space == 0) {
        return;
    }
    int n = uart_read_bytes(BUS_UART_NUM, &s_rx_stash[s_rx_stash_len], space, 0);
    if (n > 0) {
        echo_observe_locked(&s_rx_stash[s_rx_stash_len], n, s_rx_pulled);
        s_rx_pulled += n;
        s_rx_stash_len += n;
    }
}

// Stream offset of the next byte to arrive. It changes whenever a byte is
// received, whether or not anyone has pulled it yet.
static uint32_t rx_received_count(void)
{
    size_t buffered = 0;
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    uart_get_buffered_data_len(BUS_UART_NUM, &buffered);
    uint32_t count = s_rx_pulled + buffered;
    xSemaphoreGive(s_rx_lock);
    return count;
}

// Wait until no byte has been received for the idle window (plus jitter).
// Sleeps a tick whenever traffic is seen, so a busy bus doesn't spin the CPU.
// Returns false if the bus never went quiet within BUS_TX_QUIET_TIMEOUT_MS.
static bool wait_for_quiet_bus(void)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)BUS_TX_QUIET_TIMEOUT_MS * 1000;
    int64_t window_us = (int64_t)BUS_TX_IDLE_MS * 1000 +
                        esp_random() % (BUS_TX_IDLE_JITTER_MS * 1000 + 1);
    uint32_t last = rx_received_count();
    int64_t quiet_since = esp_timer_get_time();

    for (;;) {
        esp_rom_delay_us(BUS_IDLE_POLL_US);
        uint32_t now_count = rx_received_count();
        int64_t now = esp_timer_get_time();
        if (now_count != last) {
            if (now >= deadline) {
                return false;
            }
            vTaskDelay(1);
            last = rx_received_count();
            quiet_since = esp_timer_get_time();
            continue;
        }
        if (now - quiet_since >= window_us) {
            return true;
        }
    }
}

static void echo_arm(const uint8_t *data, size_t len)
{
    size_t buffered = 0;
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    uart_get_buffered_data_len(BUS_UART_NUM, &buffered);
    memcpy(s_echo_expected, data, len);
    s_echo_len = len;
    s_echo_start = s_rx_pulled + buffered;
    s_echo_state = ECHO_ARMED;
    xSemaphoreGive(s_rx_lock);
}

static void echo_disarm(void)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    s_echo_state = ECHO_IDLE;
    xSemaphoreGive(s_rx_lock);
}

// After TX completes, pull RX until the armed echo has matched or mismatched,
// or BUS_TX_ECHO_TIMEOUT_MS passes (returns ECHO_ARMED: no echo seen).
static echo_state_t echo_wait(size_t *bad_idx, uint8_t *bad_byte)
{
    int64_t start = esp_timer_get_time();
    for (;;) {
        xSemaphoreTake(s_rx_lock, portMAX_DELAY);
        rx_pull_locked();
        echo_state_t state = s_echo_state;
        *bad_idx = s_echo_bad_idx;
        *bad_byte = s_echo_bad_byte;
        xSemaphoreGive(s_rx_lock);

        if (state != ECHO_ARMED) {
            return state;
        }
        int64_t elapsed = esp_timer_get_time() - start;
        if (elapsed >= (int64_t)BUS_TX_ECHO_TIMEOUT_MS * 1000) {
            return ECHO_ARMED;
        }
        if (elapsed < BUS_ECHO_FAST_US) {
            esp_rom_delay_us(500);
        } else {
            vTaskDelay(1);
        }
    }
}

// Random backoff before retry `attempt` (1-based count of failed attempts).
static uint32_t backoff_ms(int attempt)
{
    uint32_t base = (uint32_t)BUS_TX_BACKOFF_BASE_MS << (attempt - 1);
    return base + esp_random() % (base + 1);
}

// ======================================================
// Bus (UART) init
// ======================================================

void bus_init(void)
{
    // Initialize GPIO2 (TX) to low before UART takes over
    // This ensures the NPN transistor is OFF during initialization
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUS_TX_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(BUS_TX_GPIO, 0));  // Set low
    ESP_LOGI(TAG, "GPIO%d initialized to LOW", BUS_TX_GPIO);

    uart_config_t uart_config = {
        .baud_rate = BUS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(BUS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(BUS_UART_NUM,
                                 BUS_TX_GPIO,
                                 BUS_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    // Install UART driver
    ESP_ERROR_CHECK(uart_driver_install(BUS_UART_NUM,
                                        UART_RX_BUFFER_SIZE,
                                        UART_TX_BUFFER_SIZE,
                                        0,
                                        NULL,
                                        0));

    // Invert TX so:
    //   - UART peripheral TX is inverted once,
    //   - transistor inverts again,
    //   => bus sees normal idle-high UART.
    ESP_ERROR_CHECK(uart_set_line_inverse(BUS_UART_NUM, UART_SIGNAL_TXD_INV));

    // Hand each received byte to the driver's ring buffer as it arrives,
    // rather than after 120 bytes or an RX timeout. The quiet-bus check
    // watches the ring buffer, so it must see a frame while it is in flight.
    ESP_ERROR_CHECK(uart_set_rx_full_threshold(BUS_UART_NUM, 1));

    s_rx_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_rx_lock || !s_tx_lock) {
        ESP_LOGE(TAG, "Failed to create bus locks");
        abort();
    }

    ESP_LOGI(TAG, "Bus UART initialised: UART%d, RX=%d, TX=%d, baud=%d",
             BUS_UART_NUM, BUS_RX_GPIO, BUS_TX_GPIO, BUS_BAUD_RATE);
}

// ======================================================
// Bus read
// ======================================================

int bus_read(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    // Poll rather than block in uart_read_bytes(), so s_rx_lock is only ever
    // held briefly and bytes are never in flight outside the stash's count.
    for (;;) {
        xSemaphoreTake(s_rx_lock, portMAX_DELAY);
        rx_pull_locked();
        size_t n = s_rx_stash_len < max_len ? s_rx_stash_len : max_len;
        if (n > 0) {
            memcpy(buffer, s_rx_stash, n);
            memmove(s_rx_stash, &s_rx_stash[n], s_rx_stash_len - n);
            s_rx_stash_len -= n;
        }
        xSemaphoreGive(s_rx_lock);

        if (n > 0) {
            return (int)n;
        }
        if (xTaskGetTickCount() - start >= timeout) {
            return 0;
        }
        vTaskDelay(1);
    }
}

// ======================================================
// Bus send
// ======================================================

int bus_send_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "bus_send_bytes: null or empty buffer");
        return -1;
    }

    // Format message for logging (with any secret value masked)
    char hex_log[3 * BUS_MESSAGE_MAX_SIZE + 4];
    framing_format_hex(data, (int)len, hex_log, sizeof(hex_log));

    if (len > BUS_MESSAGE_MAX_SIZE) {
        ESP_LOGE(TAG, "bus_send_bytes: %d bytes exceeds max frame size %d", (int)len, BUS_MESSAGE_MAX_SIZE);
        return -1;
    }

    ESP_LOGI(TAG, "TX: %s(%d bytes)", hex_log, (int)len);

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);

    int result = -1;
    for (int attempt = 1; attempt <= BUS_TX_MAX_ATTEMPTS; attempt++) {
        // Listen before talk: don't start on top of another device's frame
        if (!wait_for_quiet_bus()) {
            ESP_LOGW(TAG, "Bus not quiet for %d ms within %d ms, transmitting anyway",
                     BUS_TX_IDLE_MS, BUS_TX_QUIET_TIMEOUT_MS);
        }

        echo_arm(data, len);

        int written = uart_write_bytes(BUS_UART_NUM, (const char *)data, len);
        if (written < 0) {
            ESP_LOGE(TAG, "bus_send_bytes: uart_write_bytes failed");
            echo_disarm();
            break;
        }

        esp_err_t tx_err = uart_wait_tx_done(BUS_UART_NUM, pdMS_TO_TICKS(UART_TX_TIMEOUT_MS));
        if (tx_err != ESP_OK) {
            ESP_LOGE(TAG, "bus_send_bytes: uart_wait_tx_done failed: %s", esp_err_to_name(tx_err));
            echo_disarm();
            break;
        }

        // Collision detection: the bus echoes what we send, so any other
        // device talking over us shows up as a byte that differs
        size_t bad_idx;
        uint8_t bad_byte;
        echo_state_t echo = echo_wait(&bad_idx, &bad_byte);
        echo_disarm();

        if (echo == ECHO_MATCHED) {
            result = written;
            break;
        }
        if (echo == ECHO_ARMED) {
            ESP_LOGW(TAG, "No echo of our TX within %d ms - delivery unconfirmed",
                     BUS_TX_ECHO_TIMEOUT_MS);
            result = written;
            break;
        }

        if (attempt == BUS_TX_MAX_ATTEMPTS) {
            ESP_LOGE(TAG, "TX collision (attempt %d/%d): echo byte %d was %02X, expected %02X - giving up",
                     attempt, BUS_TX_MAX_ATTEMPTS, (int)bad_idx, bad_byte, data[bad_idx]);
            break;
        }
        uint32_t backoff = backoff_ms(attempt);
        ESP_LOGW(TAG, "TX collision (attempt %d/%d): echo byte %d was %02X, expected %02X - retrying in %lu ms",
                 attempt, BUS_TX_MAX_ATTEMPTS, (int)bad_idx, bad_byte, data[bad_idx],
                 (unsigned long)backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff));
    }

    xSemaphoreGive(s_tx_lock);

    if (result > 0) {
        ESP_LOGI(TAG, "TX complete: %d bytes written and transmitted", result);
        led_flash_tx();
    }

    return result;
}

int bus_send_message(const char *hex_string)
{
    if (hex_string == NULL || hex_string[0] == '\0') {
        ESP_LOGE(TAG, "bus_send_message: empty hex string");
        return -1;
    }

    uint8_t msg_buf[BUS_MESSAGE_MAX_SIZE];
    int msg_len = 0;
    const char *p = hex_string;

    // Parse hex string into bytes
    while (*p != '\0' && msg_len < (int)sizeof(msg_buf)) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') break;

        // Parse two hex digits
        char hex_byte[3] = {0};
        if (*p != '\0') {
            hex_byte[0] = *p++;
        } else {
            ESP_LOGE(TAG, "bus_send_message: incomplete hex byte at position %d", msg_len);
            return -1;
        }
        if (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            hex_byte[1] = *p++;
        } else {
            ESP_LOGE(TAG, "bus_send_message: incomplete hex byte at position %d", msg_len);
            return -1;
        }

        // Convert to byte
        char *endptr;
        unsigned long val = strtoul(hex_byte, &endptr, 16);
        if (endptr != hex_byte + 2) {
            ESP_LOGE(TAG, "bus_send_message: invalid hex at position %d ('%c%c')",
                     msg_len, hex_byte[0], hex_byte[1]);
            return -1;
        }

        msg_buf[msg_len++] = (uint8_t)val;
    }

    if (msg_len == 0) {
        ESP_LOGE(TAG, "bus_send_message: no bytes parsed");
        return -1;
    }

    return bus_send_bytes(msg_buf, msg_len);
}
