#ifndef DEVICE_SERIAL_H
#define DEVICE_SERIAL_H

#include <stddef.h>

// Crockford Base32 of a 6-byte MAC: ceil(48/5) = 10 chars + null terminator
#define DEVICE_SERIAL_LEN 11

// Last 3 MAC bytes as uppercase hex, e.g. "A1B2C3" + null terminator
#define DEVICE_MAC_SUFFIX_LEN 7

// Full MAC as colon-separated lowercase hex, e.g. "aa:bb:cc:dd:ee:ff" + null
#define DEVICE_MAC_STRING_LEN 18

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

/**
 * Get the last 3 bytes of the WiFi STA MAC address as uppercase hex,
 * e.g. "A1B2C3". Matches the suffix used in the AP SSID and mDNS hostname.
 *
 * The result is cached after the first call.
 *
 * @param buf  Output buffer (must be at least DEVICE_MAC_SUFFIX_LEN bytes)
 * @param len  Size of output buffer
 */
void device_get_mac_suffix(char *buf, size_t len);

/**
 * Get the WiFi STA MAC address as a colon-separated lowercase hex string,
 * e.g. "aa:bb:cc:dd:ee:ff". Used in the HA device registry connections field.
 *
 * The result is cached after the first call.
 *
 * @param buf  Output buffer (must be at least DEVICE_MAC_STRING_LEN bytes)
 * @param len  Size of output buffer
 */
void device_get_mac_string(char *buf, size_t len);

#endif // DEVICE_SERIAL_H
