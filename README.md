# Astral Pool Controller

Code to listen on and control an Astral Connect 10 pool controller.

This has been created by listening to the communications on the control bus, and decoding the instructions by trial and error.

## Output
To see the output, either monitor the device using the ESP monitor - or connect to the port exposed on the wifi network.

Example on a mac using nc (netcat)
```
% nc 192.168.0.222 7373
Connected to ESP32-C6 pool bus bridge.
UART bytes will be shown here in hex.
Bytes you send will be forwarded to the bus.

00
02 00 50 FF FF 80 00 FD 0F DC 19 0E 01 28 03
00
```


## Initial Provisioning:

If the LED is purple then connect to the POOL_XXXXXX wifi access point on your phone
In your phone browser navigate to http://192.168.4.1
Here you can choose the wifi network and enter the password.
This will save the details to the NVRam.

Note - if the wrong password is entered - it will try to connect for about 30 seconds and then reset to access point mode and you can start again.

Note 2: you need to clear the NVRam to redo this flow via "Erase Flash Memory from device"

## Visual Feedback Flow:
First Boot (No WiFi):
* Blue solid (startup)
* Purple solid (unconfigured detected)
* Purple solid (provisioning mode active)
* Connect to AP → Configure → Device restarts

Subsequent Boots (With WiFi):
* Blue solid (startup)
* Yellow solid (connected & got IP)
TODO - add the MQTT States



## General architecture

```mermaid
flowchart TD

    Pool[fa:fa-life-ring Pool Connect 10]

    subgraph Astral Pool Controller
        Comms[Comms Module]
        Core[fa:fa-microchip Core System]
        Web[HTTP Interface]
        Telnet[Serial Debug]
        MQTT[MQTT Client]
        LED[Led signals]
    end

    HA[Home Assistant]

    Pool <-->|fa:fa-plug via RJ12| Comms
    Comms <--> Core
    Web <--> Core
    MQTT <--> Core
    Telnet <--> Core
    LED <--> Core
    MQTT <-->|fa:fa-wifi via wifi| HA
```

The system consists of an ESP32 C6 module that can be daisy chained into and existing connect 10 system via a RJ12 connection.

It setups up a wifi AP on 192.168.4.1 for initial configuration to connect to the existing network.

It used MQTT to connect and publish information and recieve information from Home Assistant.

## Building and Flashing

This project uses ESP-IDF v5.5+. See `CLAUDE.md` for build commands and architecture details.

```bash
idf.py build          # Build the project
idf.py flash monitor  # Flash to device and monitor output
```
