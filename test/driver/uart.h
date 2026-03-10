/**
 * Mock driver/uart.h for host-based testing
 */
#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#include <stddef.h>

typedef int uart_port_t;
#define UART_NUM_1  1

int uart_write_bytes(uart_port_t uart_num, const char *src, size_t size);

#endif // DRIVER_UART_H
