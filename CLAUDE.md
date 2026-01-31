# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-C6 firmware that bridges an Astral Connect 10 pool controller's serial bus to TCP/WiFi. The device listens to the control bus, decodes proprietary protocol messages, and exposes a TCP server for monitoring and control.

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

- **main.c**: Application entry point, WiFi management, provisioning, UART initialization, HTTP/TCP server startup
- **tcp_bridge.c/.h**: TCP server (port 7373) that bridges UART data to/from network clients
- **message_decoder.c/.h**: Pattern-matching decoder for Astral protocol messages
- **pool_state.c/.h**: Global pool state structure and definitions
- **mqtt_poolclient.c/.h**: MQTT client lifecycle and connection management
- **mqtt_publish.c/.h**: MQTT publishing functions for pool state updates
- **mqtt_discovery.c/.h**: Home Assistant MQTT discovery integration
- **mqtt_commands.c/.h**: MQTT command subscription and handling
- **web_handlers.c/.h**: HTTP server endpoints (status, provisioning, MQTT config)
- **led_helper.c/.h**: WS2812 LED control for status indication

### Key Components

- **WiFi Station**: Connects to configured network, auto-reconnects with exponential backoff (max 5 retries)
- **WiFi Provisioning**: SoftAP mode with web-based WiFi credential configuration at http://192.168.4.1
- **UART Bus Interface**: 9600 baud on GPIO1 (RX) / GPIO2 (TX), TX inverted for transistor-based bus interface
- **TCP Bridge**: Port 7373, forwards UART data as hex strings to clients, accepts hex string commands from clients
- **Protocol Decoder**: Pattern-matching decoder using `memcmp()` for known message types
- **MQTT Integration**: Publishes pool state to Home Assistant, supports discovery and commands
- **HTTP API**: Web interface for status viewing and configuration
- **RGB LED**: WS2812 status indication (purple=unconfigured, yellow=connected, flashing for RX/TX)

## Configuration

Hardware pin configuration in `main/main.c`:
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
  ├─> tcp_bridge (UART <-> TCP forwarding)
  │     └─> message_decoder (decode UART messages)
  │           └─> mqtt_publish (publish state changes)
  ├─> mqtt_poolclient (MQTT connection)
  │     ├─> mqtt_discovery (Home Assistant integration)
  │     └─> mqtt_commands (handle MQTT commands)
  ├─> web_handlers (HTTP API)
  └─> led_helper (status LED)
```
