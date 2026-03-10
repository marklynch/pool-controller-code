Version 1.1.0
# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).


## [TODO]
- Sometimes messages fail - likely clashing message on bus. Make them do a retry
- Solar is not yet supported

## [Unreleased]
### Added
- Added sdkconfig.defaults to optimise build, formalise partition table, remove unused code like mdns_cli and enable use of mqtt 5
- Added log messages for discovery requests for valves and lights. Cleaned up logging messages to use info level

### Changed
### Deprecated
### Removed
### Fixed

## [0.9.0] - 2026-03-10
### Added
- Extended heater model to support up to `MAX_HEATERS` (2) heaters — replaced flat `heater_on`/`heater_valid` booleans in `pool_state_t` with a `pool_heater_t heaters[MAX_HEATERS]` array; MQTT topics are now indexed (`pool/{id}/heater/0/state`, `pool/{id}/heater/0/set`); heater discovery, state publishing, and `/status` JSON all follow the same lazy/on-discovery pattern used by channels and light zones; a stub for heater 1 logs a warning until its bus protocol is captured
- Expanded unit test coverage in `test/test_message_decoder.c` — added 6 new tests: heater OFF, current temperature reading, channel status (all off), channel status (light zones active), chlorinator pH setpoint, and chlorinator ORP setpoint; all byte vectors taken directly from real bus captures
- Added `test/test_mqtt_commands.c` — 42 host-based unit tests for `mqtt_commands.c` covering heater ON/OFF, heater-1 stub rejection, out-of-range/malformed topic handling, channel toggle, mode switch (Pool/Spa), temperature setpoint (pool and spa), light zone ON/OFF, valve On/Auto, wrong device ID routing, and unknown topic rejection; uses a UART spy to verify the exact bytes written to the bus
- Added `.vscode/tasks.json` with a `Run Tests` task so the host-based test suite can be run from VS Code via Terminal → Run Task or `⇧⌘P` → Tasks: Run Test Task

### Changed
### Deprecated
### Removed
### Fixed
- Fixed MQTT client logging a spurious "Client asked to stop, but was not started" warning on every WiFi disconnect — `mqtt_client_stop()` now tracks whether the client was ever started and skips the stop call if not, preventing the error from contributing to the retry counter
- Fixed host-based unit tests failing to compile after heater model refactor — updated `mqtt_publish_heater` mock signature, field references (`heaters[0].on`/`heaters[0].valid`), and added missing `mqtt_publish_valve` and `register_requester_notify` mocks
- Fixed test message fixtures in `test_message_decoder.c` using `0x00` for byte[9] (header checksum) — temperature and heater messages now use correct real-world byte values so patterns match and handlers execute; corrected expected device name for `0x0050` from `"Controller"` to `"Touch Screen"`
- Fixed missing `#include <stdlib.h>` in `message_decoder.c` — `strtoul`/`malloc`/`free` were relying on ESP-IDF transitive includes, causing host-based test builds to fail
- Fixed missing `#include <stddef.h>` in `main/mqtt_poolclient.h` — `size_t` was unresolved when the header was included from a host build context not using ESP-IDF transitive includes
- Fixed format specifier mismatch for `temp_c` in `mqtt_commands.c` — `strtol` returns `long` but `PRId32` expects `int32_t`; parse and range-check now use `long temp_parsed`, then assign to `int32_t temp_c` after validation so `PRId32` is correct and the cast is provably safe


## [0.8.2] - 2026-03-10
### Changed
- Migrated `mqtt_discovery.c` JSON construction from manual `snprintf` format strings to cJSON — eliminates fragile buffer sizing, stack-allocated `char device_json[512]` buffers, and heap `malloc`/`free` blocks in channel/light/valve discovery; removes `MQTT_DISCOVERY_CONFIG_SIZE` from `config.h`

### Fixed
- Migrated `/status` HTTP handler in `web_handlers.c` from ~80 `snprintf` calls to cJSON — eliminates silent truncation against a fixed 8192-byte buffer and correctly escapes channel names, valve labels, and other user-configurable strings that could contain `"` or `\`; removes `HTTP_STATUS_BUFFER_SIZE` from `config.h`
- Fixed WiFi scan results JSON in `web_handlers.c` building SSID strings with raw `snprintf` — SSIDs containing `"` or `\` produced malformed JSON; replaced with cJSON so all SSID characters are correctly escaped
- Fixed WiFi scan page not handling non-200 responses — a 500 from the server caused a cryptic JS `SyntaxError` instead of a readable message; added `r.ok` check, a "Retry Scan" button that appears on failure, and a "No networks found" fallback in the select
- Fixed `broker_uri`, `lwt_topic`, and `device_id` in `mqtt_poolclient.c` declared as stack buffers then passed as pointers to `esp_mqtt_client_init` — made `static` so their lifetime is unambiguously valid for the lifetime of the MQTT client
- Fixed `sys_table` buffer in `web_handlers.c` undersized at 700 bytes for its HTML format string — increased to 1024 to prevent silent truncation producing malformed HTML
- Fixed uninitialized `pool_state_t snapshot` passed to MQTT publish on mutex timeout in 12 handlers in `message_decoder.c` — restructured each to early-return on mutex failure so `mqtt_publish_*` is only reached when the snapshot was actually populated
- Fixed single-digit-only channel/light/valve number parsing in MQTT command handler — replaced `cmd_topic[N] - '0'` with `strtol`, validating that the parsed number is followed by `/` so multi-digit numbers and malformed topics are rejected cleanly
- Fixed fragile `hexLine` buffer arithmetic in `tcp_bridge.c` — buffer size, loop guard, and `\r\n` append now all reference the same `+3`/`-3` constant, and the append uses `hex_pos + 2` instead of mutating `hex_pos++`
- Fixed `s_log_client_sock` race in `tcp_bridge.c`: the log vprintf callback and the TCP bridge task both sent to the same client socket fd without synchronisation, causing interleaved output — all sends to `client_sock` now go through a `send_to_client` helper that holds `s_log_mutex`, serialising them with the vprintf sends
- Fixed register label loop using hardcoded `32` instead of `MAX_REGISTER_LABELS` in `message_decoder.c`, `web_handlers.c`, and `register_requester.c` — added `MAX_REGISTER_LABELS` to `config.h` and used it for the `pool_state_t` array declaration and both decoder loops
- Fixed `ESP_ERROR_CHECK` on mDNS init and service registration in `wifi_provisioning.c` — mDNS is non-critical; failures now log a warning and continue rather than rebooting the device
- Fixed `ESP_ERROR_CHECK` on WiFi scan start and result retrieval in `web_handlers.c` — a scan failure now returns HTTP 500 to the client rather than rebooting the device

### Security
- Fixed `s_last_tx_msg` loopback buffer using hardcoded `256` instead of `BUS_MESSAGE_MAX_SIZE`, which would cause silent truncation if max message size was changed
- Fixed `strcpy` after `malloc` in web handlers HTML footer helper — replaced with `memcpy` using the already-known length
- Fixed partial UART write silently treated as success in `send_uart_command` — now logs an error if fewer bytes were written than requested
- Fixed fragile `strstr`/`strchr` JSON parsing in WiFi provisioning and MQTT config HTTP handlers — replaced with cJSON for correct handling of field ordering, escaped characters, and malformed input
- Fixed `tcp_bridge_stop()` leaking the log mutex and leaving `esp_log_set_vprintf` pointing at a stale callback — now restores original vprintf and deletes the mutex on stop
- Fixed magic number temperature limits replaced with `TEMP_SETPOINT_MIN_C` (10°C) and `TEMP_SETPOINT_MAX_C` (42°C) constants shared across validation and MQTT discovery — also corrected the validation minimum which was incorrectly set to 15°C


## [0.8.1] - 2026-03-09
### Changed
- Restructured partition table to fit 4MB flash: removed factory partition, expanded OTA slots to 1.875MB each (0x1E0000)
- Simplified WiFi credential storage to use the ESP-IDF WiFi driver's built-in flash persistence instead of a separate custom NVS store

### Fixed
- Fixed SoftAP provisioning mode not starting when flash is blank: detect missing credentials in `WIFI_EVENT_STA_START` and signal provisioning immediately rather than waiting for connection retries that never fire
- Fixed printf format specifier portability issues for cross-chip compatibility (ESP32-C3/C6): use `PRId32`/`PRIu32`/`PRIX32` for `int32_t`/`uint32_t` and `%zu` for `size_t` instead of `%d`/`%lu`
- Fixed MQTT discovery to use `default_entity_id` instead of deprecated `object_id` (breaking in HA 2026.4)
- Fixed pH sensor discovery to use `device_class: ph` without `unit_of_measurement` (unit is invalid with this device class)

## [0.8.0] - 2026-03-04
### Added
- Added support for valves, including reading name, and showing a sensor for state.
- Added support for changing valve state
- Added support for E7/E8 slot 0 pool and spa set point temperature registers
- Display total channels in the touchscreen section
- Display Active state as well as status in logging of channel state
- Added checks for the header checksum and validate against field length
- Added known command bytes section to the `PROTOCOL.md` file
- Added register entries support for channel state  - Register range: `0x8C-0x93`, Slot: `0x02`
- Added support for number of channels register: `0xF4` Slot: `0x01`

### Changed
- Updated documentation based on new understanding of header checksum
- Combined register messages, timers, and register labels into a single section
- Simplified per message validation now that full data structure is known

### Fixed
- Fixed inconsistent layout between pages by removing un-needed div container
- Fixed issue where web requests were enabled due to stale connections
- Fixed MQTT discovery to use `default_entity_id` instead of deprecated `object_id` (breaking in HA 2026.4)
- Fixed pH sensor discovery to use `device_class: ph` without `unit_of_measurement` (unit is invalid with this device class)

## [0.7.0] - 2026-02-25
### Added
- Added display of serial number to home page
- Use non clashing hostname poolcontrol-AABBCC.local via mdns
- Added firmware and serial number to mDNS messages
- Added serial number, url, mac info to device in home assistant
- Added decoding for known register read requests.
- Added decoding of the firmware version of the internet gateway
- Added support for light names (and use in MQTT/HA)
- Added reading of multicolor light support
- Send requests for timers and light names if the Internet Gateway is not connected

### Changed
- Improved the naming of entities to use the dns reference in the entity id, but have a simple name
- Cleaned up the logging output to make it less verbose and easier to spot patterns
- Pulled the uart code out to bus.c

## [0.6.0] - 2026-02-22
### Added
- Added support for reading timers
- Added counting of known vs unknown messages on the bus
- Added partial support for additional temperature message from heater `31 0E 21`
- Added toggle buttons for channel to switch modes

### Removed
- Removed switches for channels as there isn't a clean interface for this

### Changed
- Tidy up naming across project

### Fixed
- Fixed MSG_DECODER: RX MSG: which was truncating the last byte
- Fixed incorrect config for Valve labels
- Fixed handler for light configuration to show on/off state
- Fixed favicon not working on iPad tabs - added png version

## [0.5.0] - 2026-02-20
### Added
- Added DNS for captive portal initial setup
- Added support for turning lights on and off via MQTT
- Added support for switching between pool/spa via MQTT
- Added new messages for changing lights and pool spa mode to protocol doc.
- Added support for toggling channel state
- Added support for Heater on/off command
- Added support for setting pool and spa setpoints
- Added favicon.ico because the 404 errors were annoying me
- Added support for static files such as css/js etc.
- Added NTP support to set real time clock correctly.
- Added local timezone from browser at bottom of status page.
- Added oat.ink styling for better consistency, performance and accessibility
- Added new home page which gives status info
- Added robots.txt to disallow indexing

### Changed
- Improved the clarity of the LED states and restore state after RX/TX
- Reworked the message decoder for channels now that we know channels are also for lights/heater.
- Pulled out more constants that were hidden or duplicated into `config.h`
- Improved mutex handling in tcp_bridge.c

### Fixed
- Added default port for MQTT
- Moved the logging earlier in the startup process to be more effective.
- Reduced used of heap for logging messages to avoid potential heap overflow.
- Fixed race condition on LED state
- Fixed -  s_mqtt_connected not marked as volatile
- Fixed abort on tx timeout - doesn't need to crash
- Fixed uninitialized struct sockaddr_in client_addr
- Fixed provisioning AP was being setup every time - even when configured.
- 404 handler only redirects in captive portal mode - otherwise proper 404.
- Fixed - don't show lights in the channels section
- Fixed up wiring for the heater on/off commands via MQTT

### Security
- Improved security of mqtt credentials password (don't send back)
- Security - Channel count not bounds-checked before array write
- Security - fix potential non-null-terminated string extraction from payload
- Security - fix potential parsing issue in MQTT channel publishing logic
- Security -  Fix missing DNS response building lacks bounds check
- Security - Improve malformed MQTT message for setting temperature

## [0.0.6] - 2026-02-09
### Added
- Added the version number to the footer of web pages
- Added support for the controller clock time
- Added support for touchscreen firmware version and display on status
- Added navigation around the Status page via `/status_view`
- Added support for Valve label message types
- Added support for requests from IG for request config 
- Added endpoint `/test_decode` to test message decoding.
- Added functions to extract UINT16_LE and UINT32_LE in message_decoder
- Added touchscreen_unknown1 and touchscreen_unknown2 messages
- Added control of log levels for each 
- Always log out unhandled messages
- Add decoding of messages sent also to allow for debugging more easily
- Added mdns for easy discovery at `poolcontrol.local`

### Removed 
- Removed unused function `decode_wrapper_for_bridge`
### Changed
- Clean up internals around web provisioning
- Pull out the wifi provisioning code from `main.c` to `wifi_provisioning.c`
- Refactor all the config variables into `config.h`
- Changed message lookup tables to use `02 00 FF` style instead of `0x02, 0x00, 0xFF` for consistent searching. 
- Refactored the `decode_message` function and split into smaller functions
- Improve the message label consistency
- Renamed MSG_TYPE_38_BASE to MSG_TYPE_CHANNEL_NAMES and use full subcommand bytes
- Reworked the REGISTER message handling based on Registers and Slots.

### Fixed
- Made the title for Wifi Config page consistent
- Improved the make process to consistently update the version number for build
- Fixed support for time which includes day of week.
- Improve the reliability of message recieving to reduce broken messages
- Bug in reading of comms status to server

## [0.0.5] - 2026-01-31

### Added
- Add support for OTA updates of firmware - see `OTA_UPDATE.md`
- Added some more status message states to code and `PROTOCOL.md`
### Fixed
- Fixed potential buffer overflow error in `bus_send_message()`
- Always set the `charset=UTF-8` on web responses.
- Fixed upload of the firmware and partitioning
- Fixed mark the new volume as valid on successful boot
- Auto reload the `/update` page on successful update.

## [0.0.4] - 2026-01-31

### Added
- Functions for sending messages
- Support for telnet sending messages.
- Do checksums on all messages
- Added checkdata.py helper script
- Added serial number for internet gateway
- Added label message type
- Added IP address and wifi signal level for internet gateway
- Added comms status for internet gateway
- Added tests for the protocol decoder
- Added PROTOCOL.md file which describes the wire protocol
- Added reading of the fahrenheit temperatures and display in status page
- Add versioning to build and status page

### Changed
- Changed MSG_TYPE_38 to MSG_TYPE_REGISTER_STATUS
- Change heater on/off to On/Off to match other values in /status page
- Improved the internal locking/mutex for MQTT publishing
- Pulled the TCP server out of main.c to own tcp_bridge.c
- Pulled the message decoder out of main.c to message_decoder.c
- Refactored handling of data blocks to be simpler
- Updated `CLAUDE.md` and `README.md` to reflect current state

### Fixed
- Publish ORP and PH setpoint to MQTT

## [0.0.3] - 2026-01-19

### Added
- Added `/status` endpoint which shows current state as json page (needs testing).
- Added initial MQTT config for home assistant configuration.
- Added `/mqtt_config` endpoint to configure mqtt.
- Added navigation to the html pages and made use common headers
- Added `current_ms` counter to status page
- Add decoding for light configured messages 
- Prepend Channel Id to the channel names

### Changed
- Pulled out led functions to helper file.
- Pulled out web handlers to own files.
- Pulled out pool state to own file.
- Refactored the MQTT state management to use the pool_state 

### Fixed
- Wifi page now deduplicates AP's and shows current network selected
- Log out the IP address correctly when it has one.
- Improved the display of Spa and Pool temps in home assistant box instead of slider
- Fixed the display of pool light in home assistant
- Only send discovery messages for channels and lights that actually are in use.
- Make heater be a switch instead of a sensor

## [0.0.2] - 2026-01-17

### Added
- Decoding of the following messages types:
- Mode (Pool, Spa)
- Lighting Zones (Off,Auto,On), color and active state
- Temperature - Spa set temp, pool set temp, current temp
- Chlorinator, pH etpoint, pH reading, ORP setpoint, ORP reading.
- Temperature Scale, Celcius or Fahrenheit
- Support for channels including lookup of names and states
- Add support for heater state On/Off
- Add logging of source and destination
- Add ability to configure wifi credentials via POOL_XXXXX Access point.
- Improve feedback from LED to show state

### Changed
- Cleanup lighting enum to be consistent with other enums instead of an embedded switch

### Removed
- Removed static config of Access point - secrets.h.example

## [0.0.1] - 2026-01-14

### Added
- Initial commit of code that can listen on the bus and output the bytes it reads
- log to the monitor console
- log to a tcp connection on port 7373