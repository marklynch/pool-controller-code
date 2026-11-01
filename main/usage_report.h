#ifndef USAGE_REPORT_H
#define USAGE_REPORT_H

#include <stdbool.h>

/**
 * Anonymous usage reporting.
 *
 * With every firmware-update check (shortly after boot, then every 12 hours)
 * the device POSTs a tiny JSON body to
 * USAGE_REPORT_URL, carrying nothing but a random install ID and the running
 * firmware version: {"id":"9f3a1c0b7e2d4a58","v":"1.8.2"}. The backend adds
 * the country and continent from the request; nothing about the pool, the
 * bus, the network or the hardware is sent.
 *
 * The install ID is 8 random bytes, made once and kept in NVS, so it survives
 * reboots and OTA updates but not a factory reset (NVS erase). It is
 * deliberately unrelated to the MAC address or the device serial.
 *
 * Reporting is fire-and-forget: a failed ping is logged at debug level and
 * dropped, with no retry, so the endpoint being down can never affect
 * anything else the device does.
 */

// Load the enabled flag and the install ID from NVS, creating the ID if this
// device has never had one. Call once at startup, after WiFi has started (so
// the hardware RNG is drawing on RF noise) and before firmware_update_init().
void usage_report_init(void);

// Send a ping if reporting is enabled. Blocks for at most one HTTP request;
// the caller must have checked that the network is up and no OTA install is
// running.
void usage_report_send(void);

bool usage_report_get_enabled(void);

// Store the setting in NVS. Turning it off takes effect from the next
// usage_report_send(); nothing is sent while it is off.
void usage_report_set_enabled(bool enabled);

#endif // USAGE_REPORT_H
