#ifndef USAGE_REPORT_H
#define USAGE_REPORT_H

#include <stdbool.h>

/**
 * Anonymous usage reporting.
 *
 * With every firmware-update check (shortly after boot, then every 12 hours)
 * the device POSTs a tiny JSON body to USAGE_REPORT_URL, carrying nothing but
 * a random install ID and the running firmware version:
 * {"id":"9f3a1c0b7e2d4a58","v":"1.8.2"}. The backend adds the country and
 * continent from the request; nothing about the pool, the bus, the network or
 * the hardware is sent.
 *
 * The install ID is 8 random bytes, made before the first ping and kept in
 * NVS, so it survives reboots and OTA updates but not a factory reset (NVS
 * erase). It is deliberately unrelated to the MAC address or the device
 * serial.
 *
 * Opting out asks the server to delete this install's rows
 * (DELETE USAGE_REPORT_URL/<id>), retried on each run until the server
 * confirms. The device keeps its ID, so opting back in later carries on with
 * the same one.
 *
 * Pings are fire-and-forget: a failed one is logged at debug level and
 * dropped, so the endpoint being down can never affect anything else the
 * device does.
 */

// Load the enabled flag, any pending deletion and the install ID from NVS.
// Call once at startup, before firmware_update_init() starts the task that
// calls usage_report_run().
void usage_report_init(void);

// Do whatever is due: ask the server to delete this install's rows if the
// user opted out, otherwise send a ping if reporting is enabled. Blocks for
// at most one HTTP request. Call only from the firmware-update check task,
// after checking that the network is up and no OTA install is running.
void usage_report_run(void);

bool usage_report_get_enabled(void);

// Store the setting in NVS. Turning it off stops pings from the next
// usage_report_run(), which also asks the server to delete this install's
// rows; call firmware_update_wake() afterwards so that happens promptly.
void usage_report_set_enabled(bool enabled);

#endif // USAGE_REPORT_H
