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
### Changed
### Deprecated
### Removed
### Fixed
- Fixed `register_requester` directly accessing global `s_pool_state` and `s_pool_state_mutex` — `register_requester_start` now accepts `pool_state_t *` and `SemaphoreHandle_t` parameters, matching the dependency-injection pattern used by the message decoder; `main.c` passes `&s_pool_state` and `s_pool_state_mutex` at startup
- Fixed `send_uart_command` in `mqtt_commands.c` bypassing `bus_send_message` — now calls `bus_send_bytes` (extracted from `bus_send_message`) so MQTT commands get TX-wait, TX LED flash, and hex logging consistent with all other bus writes; removed direct `uart_write_bytes` call and `driver/uart.h` include from `mqtt_commands.c`
- Fixed race condition in `dns_server_stop` — replaced unreliable 100ms `vTaskDelay` + conditional `vTaskDelete` with a binary semaphore; the task signals the semaphore on all exit paths before calling `vTaskDelete(NULL)`, and `dns_server_stop` blocks on it (3s timeout) rather than guessing when the task has finished
- Fixed `/status` handler holding the pool state mutex for the entire JSON build — now takes a snapshot immediately after acquiring the mutex and releases it before any cJSON allocation, eliminating contention with the message decoder under load
### Security
- Fixed provisioning request buffer too small for max-length SSID (32 bytes) + password (63 bytes) + JSON overhead — increased `HTTP_PROVISION_BUFFER_SIZE` from 200 to 512 bytes
- Fixed channel, light zone, and valve MQTT payloads using `snprintf` with unescaped `name` fields — replaced with `cJSON` construction so names containing `"`, `\`, or control characters produce valid JSON
- Fixed `handle_unknown` heap-allocating a log buffer per unknown bus message — replaced with a stack buffer sized to `3 * BUS_MESSAGE_MAX_SIZE + 1` (769 bytes), eliminating heap fragmentation risk, silent OOM discard, and the signed integer overflow in `3 * len`
- Fixed `malloc(0)` and NULL pointer passed to `esp_wifi_scan_get_ap_records` when a WiFi scan returns zero APs — now returns an empty JSON array early before the `malloc` call
- Fixed dangling pointer in `mqtt_client_init` — `config.username` and `config.password` were stack-allocated fields pointed to directly by the MQTT client config; they are now copied into static buffers (`s_username`, `s_password`) before assignment, matching the existing pattern used for `broker_uri`, `device_id`, and `lwt_topic`
- Fixed provisioning AP password being logged in plaintext at INFO level — removed password from both `ESP_LOGI` calls in `wifi_provisioning.c`, preventing it from appearing on the serial console or being forwarded to any connected TCP log client
- Fixed out-of-bounds read in `tcp_bridge_vprintf` — `vsnprintf` returns the would-be length when the buffer is too small, and that uncapped value was passed directly to `send`, reading past the end of the 256-byte stack buffer; capped to buffer size before sending
- Fixed XSS via unescaped dynamic content in HTML responses — added `html_escape()` helper and applied it to WiFi SSID and MQTT broker in the home page, and broker/username in the MQTT config form; also converted the MQTT config form's `html_fields[1536]` fixed stack buffer to a dynamically-sized heap allocation
- Fixed silent truncation of home page system info, WiFi, and MQTT rows — replaced fixed-size stack buffers (`sys_table[1024]`, `wifi_row[96]`, `mqtt_row[256]`) with heap-allocated buffers sized via `snprintf(NULL, 0, ...)`, matching the pattern used by `get_page_header`/`get_page_nav`; also explicitly null-terminates `ap_info.ssid` before use
- Fixed OTA handler accepting zero, negative, or oversized `Content-Length` values — added validation that rejects requests outside the range 1–`OTA_MAX_FIRMWARE_SIZE` (0x1E0000, matching the partition table) before entering the receive loop
- Fixed race condition in `handle_mode_control_cmd`, `handle_favourite_label`, and `handle_favourite_enable` — `mqtt_publish_favourite` was called with a raw pointer to shared pool state after the mutex was released; all three now capture a snapshot inside the mutex and pass `&state_snapshot`, consistent with every other publish call in the decoder
- Fixed out-of-bounds array writes in light zone register handlers (`handle_light_zone_state`, `_color`, `_active`, `_multicolor`, `_name`) — zone index derived from bus `reg_id` was not bounds-checked before indexing `lighting[MAX_LIGHT_ZONES]`, allowing a crafted or malformed bus packet to corrupt adjacent fields in `pool_state_t`; dispatch table `reg_end` values tightened to `base + MAX_LIGHT_ZONES - 1` and an explicit bounds check added in each handler
- Fixed potential silent truncation of MQTT broker URI — increased `broker_uri` static buffer from 192 to 256 bytes in `mqtt_poolclient.c`; the previous margin was tight enough that a max-length broker hostname with port would silently truncate the URI passed to the MQTT client

## [0.10.0] - 2026-03-12
### Added
- Added sdkconfig.defaults to optimise build, formalise partition table, remove unused code like mdns_cli and enable use of mqtt 5
- Added log messages for discovery requests for valves and lights. Cleaned up logging messages to use info level
- Added binary sensor per channel for active state (`Filter Pump Active: ON/OFF`) alongside the existing mode sensor (`Off/Auto/On/…`) — both read from the same retained MQTT state topic using `value_json.state` and `value_json.active` respectively
- Added Favourites / mode select for Home Assistant — `select` entity with dynamic options: Pool, Spa, All Auto, plus any enabled user Favourites by name; options update automatically as register data arrives
- Added decoder for favourite name registers (0x31–0x38, slot 0x03) and enable-flag registers (0x21–0x28, slot 0x03); names and enabled states stored in `pool_state_t`
- Added favourite polling to `register_requester` — requests missing name and enable-flag registers when no Internet Gateway is present
- Added favourites to `/status` JSON (enabled favourites only, showing index and name)
- Added `active_favourite` tracking: CMD 0x2A (IG→Touchscreen) decoded to record which mode/favourite is currently active; state published to `pool/{id}/favourite/state`
- Added MQTT command topic `pool/{id}/favourite/set` — accepts Pool, Spa, All Auto, or any enabled favourite name and sends the corresponding CMD 0x2A to the bus
- Added favourite discovery re-publishing when names or enable flags change
- Documented CMD 0x2A Mode/Favourite Control command and register layout (0x21–0x28, 0x31–0x38) in `PROTOCOL.md`

### Removed
- Removed dead `handle_register_label_generic` handler — superseded by the specific `handle_favourite_label` and `handle_favourite_enable` handlers; was never re-registered in `REGISTER_HANDLERS` after that refactor

## [0.9.0] - 2026-03-10
### Added
- Extended heater model to support up to `MAX_HEATERS` (2) heaters — replaced flat `heater_on`/`heater_valid` booleans in `pool_state_t` with a `pool_heater_t heaters[MAX_HEATERS]` array; MQTT topics are now indexed (`pool/{id}/heater/0/state`, `pool/{id}/heater/0/set`); heater discovery, state publishing, and `/status` JSON all follow the same lazy/on-discovery pattern used by channels and light zones; a stub for heater 1 logs a warning until its bus protocol is captured
- Expanded unit test coverage in `test/test_message_decoder.c` — added 6 new tests: heater OFF, current temperature reading, channel status (all off), channel status (light zones active), chlorinator pH setpoint, and chlorinator ORP setpoint; all byte vectors taken directly from real bus captures
- Added `test/test_mqtt_commands.c` — 42 host-based unit tests for `mqtt_commands.c` covering heater ON/OFF, heater-1 stub rejection, out-of-range/malformed topic handling, channel toggle, mode switch (Pool/Spa), temperature setpoint (pool and spa), light zone ON/OFF, valve On/Auto, wrong device ID routing, and unknown topic rejection; uses a UART spy to verify the exact bytes written to the bus
- Added `.vscode/tasks.json` with a `Run Tests` task so the host-based test suite can be run from VS Code via Terminal → Run Task or `⇧⌘P` → Tasks: Run Test Task

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