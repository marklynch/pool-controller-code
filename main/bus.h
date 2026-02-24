#ifndef BUS_H
#define BUS_H

// Send a hex-encoded message to the RS485 bus.
// hex_string: space-separated or packed hex bytes, e.g. "02 00 F0 FF FF 80 00 39 0E B7 08 04 0C 03"
// Returns the number of bytes sent, or -1 on parse error.
int bus_send_message(const char *hex_string);

#endif // BUS_H
