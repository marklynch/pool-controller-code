#include "bus.h"
#include "config.h"
#include "led_helper.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BUS";
static SemaphoreHandle_t s_tx_mutex = NULL;
// Keep synchronous MQTT/TCP callers responsive even when legacy config values
// allow a longer arbitration wait.
#define BUS_SYNC_WAIT_CAP_MS 250U
// Low 32 bits of esp_timer_get_time(). Unsigned subtraction remains valid
// across wrap-around for the short guard/deadline intervals used here.
static volatile uint32_t s_last_rx_us = 0;

static uint32_t bus_time_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

static void note_rx_activity(uint32_t now_us)
{
    __atomic_store_n(&s_last_rx_us, now_us, __ATOMIC_RELEASE);
}

static bool wait_for_bus_idle(void)
{
    const uint32_t guard_us = BUS_IDLE_GUARD_MS * 1000U;
    const uint32_t timeout_ms = BUS_IDLE_WAIT_TIMEOUT_MS < BUS_SYNC_WAIT_CAP_MS
                                    ? BUS_IDLE_WAIT_TIMEOUT_MS : BUS_SYNC_WAIT_CAP_MS;
    const uint32_t timeout_us = timeout_ms * 1000U;
    const uint32_t start_us = bus_time_us();

    while (1) {
        // Do not rely only on bus_read(): the TCP bridge is both the UART
        // reader and one of the senders, so it cannot update a read-side
        // timestamp while blocked here. Looking at the driver's RX buffer
        // observes a frame arriving while any caller waits to transmit.
        size_t buffered = 0;
        esp_err_t err = uart_get_buffered_data_len(BUS_UART_NUM, &buffered);
        uint32_t now_us = bus_time_us();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot inspect UART RX buffer: %s", esp_err_to_name(err));
            return false;
        }
        if (buffered > 0) {
            note_rx_activity(now_us);
        }

        uint32_t last_rx_us = __atomic_load_n(&s_last_rx_us, __ATOMIC_ACQUIRE);
        uint32_t quiet_us = now_us - last_rx_us;
        if (buffered == 0 && quiet_us >= guard_us) {
            return true;
        }
        uint32_t elapsed_us = now_us - start_us;
        if (elapsed_us >= timeout_us) {
            return false;
        }

        // Sleep until the current quiet period could satisfy the guard (or
        // the overall deadline), rather than waking on every RTOS tick. The
        // elapsed time is still measured in microseconds, so a 100 Hz tick
        // cannot shorten a 12 ms guard to one tick.
        uint32_t quiet_remaining_us = quiet_us < guard_us ? guard_us - quiet_us : 1000U;
        uint32_t deadline_remaining_us = timeout_us - elapsed_us;
        uint32_t sleep_us = quiet_remaining_us < deadline_remaining_us
                                ? quiet_remaining_us : deadline_remaining_us;
        uint32_t sleep_ms = (sleep_us + 999U) / 1000U;
        TickType_t sleep_ticks = pdMS_TO_TICKS(sleep_ms);
        vTaskDelay(sleep_ticks > 0 ? sleep_ticks : 1);
    }
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

    s_tx_mutex = xSemaphoreCreateMutex();
    if (s_tx_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create bus TX mutex");
        abort();
    }
    note_rx_activity(bus_time_us());

    ESP_LOGI(TAG, "Bus UART initialised: UART%d, RX=%d, TX=%d, baud=%d",
             BUS_UART_NUM, BUS_RX_GPIO, BUS_TX_GPIO, BUS_BAUD_RATE);
}

// ======================================================
// Bus read
// ======================================================

int bus_read(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    int count = uart_read_bytes(BUS_UART_NUM, buffer, max_len,
                                timeout_ms / portTICK_PERIOD_MS);
    if (count > 0) {
        note_rx_activity(bus_time_us());
    }
    return count;
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

    // Format the log line before the final idle check. Formatting and logging
    // after declaring the bus idle would enlarge the race window before the
    // UART write begins.
    char hex_log[3 * BUS_MESSAGE_MAX_SIZE + 4];
    int pos = 0;
    for (size_t i = 0; i < len; i++) {
        int remaining = (int)sizeof(hex_log) - pos;
        if (remaining < 4) break;
        int written = snprintf(&hex_log[pos], remaining, "%02X ", data[i]);
        if (written < 0) break;
        pos += written;
        if (pos >= (int)sizeof(hex_log)) {
            pos = (int)sizeof(hex_log) - 1;
            break;
        }
    }
    if (pos >= (int)sizeof(hex_log)) pos = (int)sizeof(hex_log) - 1;
    hex_log[pos] = '\0';

    const uint32_t lock_timeout_ms = BUS_TX_LOCK_TIMEOUT_MS < BUS_SYNC_WAIT_CAP_MS
                                         ? BUS_TX_LOCK_TIMEOUT_MS : BUS_SYNC_WAIT_CAP_MS;
    if (s_tx_mutex == NULL ||
        xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(lock_timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "TX skipped: transmitter busy");
        return -1;
    }

    if (!wait_for_bus_idle()) {
        ESP_LOGW(TAG, "TX skipped: bus did not remain idle for %d ms",
                 BUS_IDLE_GUARD_MS);
        xSemaphoreGive(s_tx_mutex);
        return -1;
    }

    // Start the UART write immediately after the successful idle check.
    int written = uart_write_bytes(BUS_UART_NUM, (const char *)data, len);
    ESP_LOGI(TAG, "TX: %s(%d bytes)", hex_log, (int)len);
    if (written != (int)len) {
        ESP_LOGE(TAG, "bus_send_bytes: uart_write_bytes wrote %d of %d bytes",
                 written, (int)len);
        xSemaphoreGive(s_tx_mutex);
        return -1;
    }

    // Wait for TX to complete
    esp_err_t tx_err = uart_wait_tx_done(BUS_UART_NUM, pdMS_TO_TICKS(UART_TX_TIMEOUT_MS));
    if (tx_err != ESP_OK) {
        ESP_LOGE(TAG, "bus_send_bytes: uart_wait_tx_done failed: %s", esp_err_to_name(tx_err));
        xSemaphoreGive(s_tx_mutex);
        return -1;
    }

    ESP_LOGI(TAG, "TX complete: %d bytes written and transmitted", written);

    led_flash_tx();

    xSemaphoreGive(s_tx_mutex);

    return written;
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
