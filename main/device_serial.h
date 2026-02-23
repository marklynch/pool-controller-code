#ifndef DEVICE_SERIAL_H
#define DEVICE_SERIAL_H

#include <stddef.h>

// Crockford Base32 of a 6-byte MAC: ceil(48/5) = 10 chars + null terminator
#define DEVICE_SERIAL_LEN 11

/**
 * Get the device serial number as a 10-character Crockford Base32 string
 * derived from the factory eFuse MAC address.
 *
 * The result is cached after the first call.
 *
 * @param buf  Output buffer (must be at least DEVICE_SERIAL_LEN bytes)
 * @param len  Size of output buffer
 */
void device_get_serial(char *buf, size_t len);

#endif // DEVICE_SERIAL_H
