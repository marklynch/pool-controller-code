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

**Single-file firmware** (`main/main.c`) with these components:

- **WiFi Station**: Connects to configured network, auto-reconnects on disconnect
- **UART Bus Interface**: 9600 baud on GPIO1 (RX) / GPIO2 (TX), TX inverted for transistor-based bus interface
- **TCP Server**: Port 7373, bridges UART data to clients, accepts commands from clients
- **Protocol Decoder**: Pattern-matching decoder for Astral bus messages
- **RGB LED**: WS2812 on GPIO8 for status indication (blue=startup, red=RX, green=TX)

## Configuration

Hardware pin configuration at top of `main/main.c`:
- `BUS_TX_GPIO`, `BUS_RX_GPIO` - UART pins
- `LED_GPIO` - WS2812 LED pin
- `TCP_PORT` - Server port (default 7373)

## Protocol Decoding

The full protocol desciption is in the `PROTOCOL.md` file.

Message patterns are defined as byte arrays (e.g., `MSG_TYPE_TEMP_SETTING[]`). The `decode_message()` function uses `memcmp()` to match incoming data against known patterns and logs decoded values.

To add a new message decoder:
1. Define the pattern as a `static const uint8_t[]`
2. Add a matching case in `decode_message()` using `memcmp()`
3. Extract and log relevant fields with `ESP_LOGI()`
4. Return `true` if decoded (suppresses raw hex output), `false` otherwise
