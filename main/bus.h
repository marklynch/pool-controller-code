#ifndef BUS_H
#define BUS_H

#include <stdint.h>
#include <stddef.h>

// Initialise the bus UART (GPIO, driver, TX inversion).
void bus_init(void);

// Read bytes from the bus UART. Returns number of bytes read.
int bus_read(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);

// Parse a hex-encoded message and send it to the bus UART.
// hex_string: space-separated or packed hex bytes,
//   e.g. "02 00 F0 FF FF 80 00 39 0E B7 08 04 0C 03"
// Returns the number of bytes sent, or -1 on parse error.
int bus_send_message(const char *hex_string);

#endif // BUS_H
