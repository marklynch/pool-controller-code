#include "bus.h"
#include "config.h"
#include "led_helper.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "BUS";

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

    ESP_LOGI(TAG, "Bus UART initialised: UART%d, RX=%d, TX=%d, baud=%d",
             BUS_UART_NUM, BUS_RX_GPIO, BUS_TX_GPIO, BUS_BAUD_RATE);
}

// ======================================================
// Bus read
// ======================================================

int bus_read(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    return uart_read_bytes(BUS_UART_NUM, buffer, max_len, timeout_ms / portTICK_PERIOD_MS);
}

// ======================================================
// Bus send
// ======================================================

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

    // Format message for logging
    char hex_log[3 * sizeof(msg_buf) + 4];
    int pos = 0;
    for (int i = 0; i < msg_len; i++) {
        int remaining = sizeof(hex_log) - pos;
        if (remaining < 4) {
            break;
        }
        int written = snprintf(&hex_log[pos], remaining, "%02X ", msg_buf[i]);
        if (written < 0) {
            ESP_LOGE(TAG, "bus_send_message: snprintf encoding error");
            break;
        }
        pos += written;
        if (pos >= (int)sizeof(hex_log)) {
            pos = sizeof(hex_log) - 1;
            break;
        }
    }
    if (pos >= (int)sizeof(hex_log)) {
        pos = sizeof(hex_log) - 1;
    }
    hex_log[pos] = '\0';

    ESP_LOGI(TAG, "TX: %s(%d bytes)", hex_log, msg_len);

    // Send to UART
    int written = uart_write_bytes(BUS_UART_NUM, (const char *)msg_buf, msg_len);
    if (written < 0) {
        ESP_LOGE(TAG, "bus_send_message: uart_write_bytes failed");
        return -1;
    }

    // Wait for TX to complete
    esp_err_t tx_err = uart_wait_tx_done(BUS_UART_NUM, pdMS_TO_TICKS(UART_TX_TIMEOUT_MS));
    if (tx_err != ESP_OK) {
        ESP_LOGE(TAG, "uart_wait_tx_done failed: %s", esp_err_to_name(tx_err));
        return -1;
    }

    ESP_LOGI(TAG, "TX complete: %d bytes written and transmitted", written);

    led_flash_tx();

    return written;
}
