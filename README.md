# Astral Pool Controller

Code to listen on and control an Astral Connect 10 pool controller.

This has been created by listening to the communications on the control bus, and decoding the instructions by trial and error.

## Setup

1. Copy `main/secrets.h.example` to `main/secrets.h`
2. Edit `main/secrets.h` with your WiFi credentials
3. Build and flash with ESP-IDF:
   ```
   idf.py build
   idf.py flash
   idf.py monitor
   ```

## Configuration

WiFi credentials are stored in `main/secrets.h` (excluded from git):

```c
#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASS       "your_wifi_password"
```

Other settings can be configured in `main/main.c`:
- `TCP_PORT` - TCP server port (default: 7373)
- `BUS_BAUD_RATE` - UART baud rate (default: 9600)
- `BUS_TX_GPIO` / `BUS_RX_GPIO` - GPIO pins for bus communication

## Usage

The device connects to WiFi and starts a TCP server. Bus traffic is logged to the serial monitor. Connect via TCP to see bus traffic and send bytes to the bus.

## Project Structure

```
├── CMakeLists.txt
├── main
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── secrets.h           WiFi credentials (not in git)
│   └── secrets.h.example   Template for secrets.h
└── README.md
```
