/**
 * Mock driver/uart.h for host-based testing
 */
#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int uart_port_t;
#define UART_NUM_1  1

typedef struct {
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    int source_clk;
} uart_config_t;

#define UART_DATA_8_BITS          8
#define UART_PARITY_DISABLE       0
#define UART_STOP_BITS_1          1
#define UART_HW_FLOWCTRL_DISABLE  0
#define UART_SCLK_DEFAULT         0
#define UART_PIN_NO_CHANGE       -1
#define UART_SIGNAL_TXD_INV       1

int uart_write_bytes(uart_port_t uart_num, const char *src, size_t size);
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, uint32_t ticks_to_wait);
esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size);
esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *config);
esp_err_t uart_set_pin(uart_port_t uart_num, int tx, int rx, int rts, int cts);
esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size,
                              int tx_buffer_size, int queue_size,
                              void *uart_queue, int intr_alloc_flags);
esp_err_t uart_set_line_inverse(uart_port_t uart_num, uint32_t inverse_mask);
esp_err_t uart_wait_tx_done(uart_port_t uart_num, uint32_t ticks_to_wait);

#endif // DRIVER_UART_H
