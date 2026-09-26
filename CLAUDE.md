# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-C6 firmware that bridges an Connect 10 pool controller's serial bus to TCP/WiFi. The device listens to the control bus, decodes proprietary protocol messages, and exposes a TCP server for monitoring and control.

## Build Commands

```bash
idf.py build          # Build the project
idf.py flash          # Flash to device
idf.py flash monitor  # Build, flash, and monitor serial output
idf.py fullclean      # Clean build
```

Requires ESP-IDF v5.5+ with environment sourced (`. $IDF_PATH/export.sh`).

## Architecture

**Modular firmware** with the following structure:

### Core Modules (`main/` directory)

- **main.c**: Application entry point and startup sequencing. Startup does not block on the network — it waits a bounded `WIFI_STARTUP_WAIT_MS` for an address, then brings up the bus bridge and local services regardless
- **wifi_provisioning.c/.h**: WiFi station lifecycle, reconnect backoff, mDNS, HTTP server startup, and the rescue portal. A supervisor task owns portal start/stop, since switching WiFi mode from the event-loop task can wedge it
- **bus.c/.h**: UART bus interface — init, read, and hex-string send (`bus_init`, `bus_read`, `bus_send_message`)
- **tcp_bridge.c/.h**: TCP server (port 7373) that bridges UART data to/from network clients
- **message_decoder.c/.h**: Pattern-matching decoder for protocol messages
- **pool_state.c/.h**: Global pool state structure and definitions
- **channel_states.c**: Channel state name table (CMD 0x0B code space), shared by the decoder, the web UI and the MQTT pump-mode select
- **filter_pump_type.c/.h**: Learns whether the Filter channel drives a single- or multi-speed pump, from the states CMD 0x0B reports for it, and persists the answer in NVS. Decides the pump-mode select's MQTT option list
- **register_requester.c/.h**: Proactively sends CMD 0x39 register read requests when Internet Gateway is absent; woken immediately when a new light zone is configured. Also serves one-off read-backs queued after a register write (`register_requester_read_back`), regardless of gateway presence
- **mqtt_poolclient.c/.h**: MQTT client lifecycle and connection management
- **mqtt_publish.c/.h**: MQTT publishing functions for pool state updates
- **mqtt_discovery.c/.h**: Home Assistant MQTT discovery integration
- **mqtt_commands.c/.h**: MQTT command subscription and handling
- **firmware_update.c/.h**: GitHub release checker + pull-based OTA. Periodically reads the repo's recent release tags from the streamed `releases.atom` feed (latest + prior versions), compares the newest against the running build, and can install any tracked release's update binary over HTTPS (`esp_https_ota`). Exposes status + the version list to the web UI and the Home Assistant `update` entity
- **web_handlers.c/.h**: HTTP server endpoints (status, provisioning, MQTT config)
- **led_helper.c/.h**: WS2812 LED control for status indication
- **heap_monitor.c/.h**: Low-priority task that periodically logs heap stats (free, min-free watermark, largest block) for leak/fragmentation diagnostics

### Key Components

- **WiFi Station**: Connects to configured network and reconnects indefinitely with capped exponential backoff. Stored credentials are never cleared automatically — an unreachable AP is not evidence they are wrong
- **WiFi Provisioning**: Rescue portal (SoftAP) with web-based credential configuration at http://192.168.4.1. Runs in APSTA alongside a still-retrying station and is torn down once that connects, so it is never a terminal state. Raised when no credentials are stored, or after the grace period in `config.h` (shorter when the AP actively rejects the credentials)
- **Bus Interface**: 9600 baud on GPIO1 (RX) / GPIO2 (TX), TX inverted for transistor-based bus interface; encapsulated in `bus.c`
- **TCP Bridge**: Port 7373, forwards UART data as hex strings to clients, accepts hex string commands from clients
- **Protocol Decoder**: Pattern-matching decoder using `memcmp()` for known message types
- **MQTT Integration**: Publishes pool state to Home Assistant, supports discovery and commands
- **HTTP API**: Web interface for status viewing and configuration
- **RGB LED**: WS2812 status indication (purple=unconfigured, yellow=connected, flashing for RX/TX)

## Configuration

Hardware pin configuration in `main/config.h`:
- `BUS_TX_GPIO`, `BUS_RX_GPIO` - UART pins (GPIO2, GPIO1)
- `TCP_PORT` - Server port (default 7373)

LED configuration in `led_helper.c`:
- `LED_GPIO` - WS2812 LED pin (GPIO8)

## Protocol Decoding

The full protocol description is in the `PROTOCOL.md` file.

Message patterns are defined as byte arrays in `message_decoder.c` (e.g., `MSG_TYPE_TEMP_SETTING[]`). The `decode_message()` function uses `memcmp()` to match incoming data against known patterns, updates pool state, and publishes to MQTT.

To add a new message decoder:
1. Define the pattern as a `static const uint8_t[]` in `message_decoder.c`
2. Add a matching case in `decode_message()` using `memcmp()`
3. Extract relevant fields and update `pool_state_t` structure (protected by mutex)
4. Optionally publish state changes via `mqtt_publish_*()`
5. Return `true` if decoded (suppresses raw hex output), `false` otherwise

## Module Dependencies

```
main.c
  ├─> bus (UART init, read, send)
  ├─> tcp_bridge (bus <-> TCP forwarding)
  │     └─> message_decoder (decode bus messages)
  │           ├─> mqtt_publish (publish state changes)
  │           ├─> filter_pump_type (learn the filter pump type from CMD 0x0B)
  │           └─> register_requester (notify on new light zone)
  ├─> register_requester (auto-poll missing registers when GW absent)
  │     └─> bus (send CMD 0x39 requests)
  ├─> mqtt_poolclient (MQTT connection)
  │     ├─> mqtt_discovery (Home Assistant integration)
  │     │     └─> filter_pump_type (pump-mode select option list)
  │     ├─> mqtt_commands (handle MQTT commands)
  │     │     └─> register_requester (read a register back after writing it)
  │     └─> firmware_update (publish update state, handle install command)
  ├─> firmware_update (GitHub release check + pull OTA)
  │     └─> mqtt_poolclient (publish HA update entity state)
  ├─> web_handlers (HTTP API)
  │     └─> firmware_update (check/install from GitHub)
  ├─> led_helper (status LED)
  └─> heap_monitor (periodic heap-stats logging)
```
