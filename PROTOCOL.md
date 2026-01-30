# Astral Pool Controller Protocol Documentation

This document describes the proprietary serial protocol used by the Astral Connect 10 pool controller.

## Message Structure

All messages follow this basic structure:

```
[START] [SRC_HI] [SRC_LO] [DST_HI] [DST_LO] [CTRL_HI] [CTRL_LO] [CMD] [SUBCMD] [DATA...] [CHECKSUM] [END]
```

### Message Format

| Offset | Field    | Description                                                   |
| ------ | -------- | ------------------------------------------------------------- |
| 0      | START    | Always `0x02`                                                 |
| 1-2    | SOURCE   | Source device address (big endian)                            |
| 3-4    | DEST     | Destination device address (big endian)                       |
| 5-6    | CONTROL  | Control bytes (typically `0x80 0x00`)                         |
| 7+     | COMMAND  | Command and subcommand bytes                                  |
| 10+    | DATA     | Payload data (varies by message type)                         |
| N-2    | CHECKSUM | Sum of all data bytes (from index 10 to N-3) masked with 0xFF |
| N-1    | END      | Always `0x03`                                                 |

### Checksum Calculation

The checksum is calculated as the sum of all bytes from index 10 to (length - 3), masked to 8 bits:

```c
uint32_t sum = 0;
for (int i = 10; i < len - 2; i++) {
    sum += data[i];
}
uint8_t checksum = sum & 0xFF;
```

### Device Addresses

| Address  | Device       | Description                       |
| -------- | ------------ | --------------------------------- |
| `0x0050` | Controller   | Main pool controller (Connect 10) |
| `0x0062` | Temp Sensor  | Temperature sensor module         |
| `0x006F` | Touch Screen | Touch screen interface            |
| `0x0090` | Chlorinator  | Chemistry/chlorinator module      |
| `0x00F0` | Internet GW  | Internet gateway module           |
| `0xFFFF` | Broadcast    | Broadcast to all devices          |

## Message Types

### 1. Mode Message (Spa/Pool)

Reports the current operating mode - pool or spa.

**Pattern:** `02 00 50 FF FF 80 00 14 0D F1`

**Example - Spa Mode:**

```
02 00 50 FF FF 80 00 14 0D F1 00 00 03
                              ^^
                              Mode: 0x00 = Spa, 0x01 = Pool
```

**Example - Pool Mode:**

```
02 00 50 FF FF 80 00 14 0D F1 01 01 03
```

**Data Fields:**

- Byte 10: Mode (`0x00` = Spa, `0x01` = Pool)

---

### 2. Temperature Settings

Reports the temperature setpoints for both spa and pool.

**Pattern:** `02 00 50 FF FF 80 00 17 10 F7`

**Example:**

```
02 00 50 FF FF 80 00 17 10 F7 25 1D 63 54 F9 03
                              ^^ Spa setpoint Celcius (37°C in this example)
                                 ^^ Pool setpoint Celcius (25°C in this example)
                                    ^^ Spa setpoint Fahrenheit (99°F in this example)
                                       ^^ Pool setpoint Fahrenheit (84°F in this example)                         
```

**Data Fields:**

- Byte 10: Spa setpoint temperature Celcius
- Byte 11: Pool setpoint temperature Celcius
- Byte 12: Spa setpoint temperature Fahrenheit
- Byte 13: Pool setpoint temperature Fahrenheit

**Note:** Temperature scale (Celsius/Fahrenheit) is set by configuration message.

---

### 3. Temperature Reading

Current water temperature from the sensor.

**Pattern:** `02 00 62 FF FF 80 00 16 0E 06`

**Example:**

```
02 00 62 FF FF 80 00 16 0E 06 19 00 19 03
                              ^^ Current temperature (25°C)
                                 ^^ Unknown
```

**Data Fields:**

- Byte 10: Current water temperature
- Byte 11: Unknown - could be secondary temp sensor

---

### 4. Heater Status

Reports whether the heater is on or off.

**Pattern:** `02 00 62 FF FF 80 00 12 0F`

**Example - Heater On:**

```
02 00 62 FF FF 80 00 12 0F 03 00 01 08 09 03
                                 ^^ 0x01 = On, 0x00 = Off
                                    ^^ Unknown
```

**Example - Heater Off:**

```
02 00 62 FF FF 80 00 12 0F 03 00 00 08 08 03
                                 ^^ 0x01 = On, 0x00 = Off
                                    ^^ Unknown
```

**Data Fields:**

- Byte 10: Padding/unused
- Byte 11: Heater state (`0x00` = Off, `0x01` = On)
- Byte 12: Unknown (maybe bitmask or interlock?)
---

### 5. Configuration

System configuration including temperature scale.

**Pattern:** `02 00 50 FF FF 80 00 26 0E 04`

**Example - Celsius:**

```
02 00 50 FF FF 80 00 26 0E 04 01 06 07 03
                              ^^ 0x01 - Celcius
                                 ^^ Unknown
```

**Example - Fahrenheit:**
```
02 00 50 FF FF 80 00 26 0E 04 11 06 17 03
                              ^^ 0x11 - Fahrenheit
                                 ^^ Unknown
```

**Data Fields:**

- Byte 10: Temperature configuration - 01 Celcius, 11, Fahrenheit - possible bitmask as defined below.
  - Bit 7: 
  - Bit 6: 
  - Bit 5: 
  - Bit 4:  0: celsius, 1:fahrenheit
  - Bit 3:  
  - Bit 2: 
  - Bit 1: 
  - Bit 0:  1:heat 0: cool
- Byte 11: Unknown
---

### 6. Active Channels Bitmask

Reports which channels are currently active.

**Pattern:** `02 00 50 00 6F 80 00 0D 0D 5B`

**Example:**

```
02 00 50 00 6F 80 00 0D 0D 5B 10 10 03
                              ^^
                              Bitmask: 0x10 = Channel 5 active
```

**Data Fields:**

- Byte 10: Channel bitmask
  - Bit 7: Channel 8
  - Bit 6: Channel 7
  - Bit 5: Channel 6
  - Bit 4: Channel 5
  - Bit 3: Channel 4
  - Bit 2: Channel 3
  - Bit 1: Channel 2
  - Bit 0: Channel 1

---

### 7. Channel Status

Detailed status for all configured channels.

**Pattern:** `02 00 50 FF FF 80 00 0B 25 00`

**Example:**

```
02 00 50 FF FF 80 00 0B 25 00 08 01 00 00 02 00 00 FE 00 00 FE 00 00 0B 02 01 09 00 00 FD 00 00 00 00 00 1B 03
                              ^^ Number of channels
                                 ^^  Channel 1: Type=1 (Filter)
                                    ^^ Channel 1: State (00 off, 01, Auto, 02 One)
                                       ^^ Channel 1:  currently active (either on or auto timer)
                                         ^^ Channel 2: Type=2 (Cleaner) 
                                            etc
```

**Data Fields:**

- Byte 10: Number of channels
- Bytes 11+: For each channel (3 bytes):
  - Byte 0: Channel type
  - Byte 1: Channel state
  - Byte 2: Additional data

**Channel Types:**

- `0x00`: Unknown
- `0x01`: Filter
- `0x02`: Cleaning
- `0x03`: Heater Pump
- `0x04`: Booster
- `0x05`: Waterfall
- `0x06`: Fountain
- `0x07`: Spa Pump
- `0x08`: Solar
- `0x09`: Blower
- `0x0A`: Swimjet
- `0x0B`: Jets
- `0x0C`: Spa Jets
- `0x0D`: Overflow
- `0x0E`: Spillway
- `0x0F`: Audio
- `0x11`: Hot Seat
- `0x12`: Heater Power
- `0x13`: Custom Name
- `0xFE`: Unused channel (disabled)
- `0xFD`: End marker (no more channels beyond this)

**Channel States:**

- `0x00`: Off
- `0x01`: Auto
- `0x02`: On

---

### 8. Channel Type Registers

Configures the registers for each channel.  Handles lights and channels

**Pattern:** `02 00 50 FF FF 80 00 38 0F 17`


**Example - Light zone:**

```
02 00 50 FF FF 80 00 38 0F 17 C0 01 00 C1 03
                              ^^ Light Zone 1 (0xC0)
                                 ^^ Unknown 
                                    ^^ Light status (off)

02 00 50 FF FF 80 00 38 0F 17 C0 01 02 C3 03
                              ^^ Light Zone 1 (0xC0)
                                 ^^ Unknown 
                                    ^^ Light status (on)
```

**Data Fields:**

- Byte 10: Channel type ID (`0xC0` to `0xC3` for light zones 1-4)
- Byte 11: Unknown (maybe type) 
- Byte 12: State (0: Off, 1: Auto, 2: Off)


**Example - Channel zone:**

```
02 00 50 FF FF 80 00 38 0F 17 6C 02 01 6F 03
                              ^^ Channel 1  (0x6C)
                                 ^^ Channel type (lookup table)
                                    ^^ Unknown
```


**Data Fields:**

- Byte 10: Channel type ID (`0x6C` to `0x73` for channels 1-8)
- Byte 11: Padding
- Byte 12: Channel type code

**Channel Type ID Mapping:**

- `0x6C`: Channel 1 type (see channel type lookup table)
- `0x6D`: Channel 2 type
- `0x6E`: Channel 3 type
- `0x6F`: Channel 4 type
- `0x70`: Channel 5 type
- `0x71`: Channel 6 type
- `0x72`: Channel 7 type
- `0x73`: Channel 8 type

- `0xC0`: Light Zone 1 State
- `0xC1`: Light Zone 2 State
- `0xC2`: Light Zone 3 State
- `0xC3`: Light Zone 4 State

- `0xD0`: Light Zone 1 Color (maybe)
- `0xD1`: Light Zone 2 Color (maybe)
- `0xD2`: Light Zone 3 Color (maybe)
- `0xD3`: Light Zone 4 Color (maybe)

- `0xE0`: Light Zone 1 Active (0: Off, 1: On)
- `0xE1`: Light Zone 2 Active
- `0xE2`: Light Zone 3 Active
- `0xE3`: Light Zone 4 Active

---

### 9. Register Labels

Generic register label assignments.

**Pattern: Cahnnels?** `02 00 50 FF FF 80 00 38 1A 22`

**Example:**

```
02 00 50 FF FF 80 00 38 1A 22 7C 02 46 69 6C 74 65 72 20 50 75 6D 70 00 A6 03
                              ^^ Register ID (0x7C)
                                ^^  Unknown
                                    F  i  l  t  e  r     P  u  m  p  (null terminated)

```
**Data Fields:**

- Byte 10: Register ID
- Byte 11: Unknown
- Byte 12+: ASCII label string (null terminated)


**Pattern Variant: Valves?** `02 00 50 FF FF 80 00 38 16 1E`
**Example: 38 16 1E**
```
02 00 50 FF FF 80 00 38 16 1E D0 02 56 61 6C 76 65 20 31 00 21 03
                              ^^ Register ID (0xD0)
                                 ^^  Unknown
                                    V  a  l  v  e     1  (null terminated)

02 00 50 FF FF 80 00 38 16 1E D1 02 56 61 6C 76 65 20 32 00 23 03
                              ^^ Register ID (0xD1)
                                 ^^  Unknown
                                    V  a  l  v  e     2  (null terminated)
```


---

### 10. Lighting Zone Configuration

Indicates which lighting zones are installed.

**Pattern:** `02 00 50 FF FF 80 00 06 0E E4`

**Example:**

```
02 00 50 FF FF 80 00 06 0E E4 00 00 00 03
                              ^^ Zone index (0-3 for zones 1-4)
                                 ^^ Unknown
```

**Data Fields:**

- Byte 10: Zone index (`0x00` to `0x03` for zones 1-4)
- Byte 11: Unknown

---

### 11. Chlorinator pH Setpoint

Target pH level for the chlorinator.

**Pattern:** `02 00 90 FF FF 80 00 1D 0F 3C` (followed by register)

**Example:**

```
02 00 90 FF FF 80 00 1D 0F 3C 01 4E 00 4F 03
                              ^^ PH Setpoint
                                 ^^ ^^ pH value (little endian)
                                          78 = 7.8 pH (value / 10)
```

**Data Fields:**

- Byte 10: pH setpoint register (0x01)
- Bytes 11-12: pH value in tenths (little endian, divide by 10 for actual pH)

---

### 12. Chlorinator pH Reading

Current pH reading from the sensor.

**Pattern:** `02 00 90 FF FF 80 00 1F 0F 3E` (followed by register)

**Example:**

```
02 00 90 FF FF 80 00 1F 0F 3E 01 55 00 56 03
                              ^^ pH reading
                                 ^^ ^^ pH value (little endian)
                                          85 = 8.5 pH
```

**Data Fields:**

- Byte 10: pH setpoint register (0x01)
- Bytes 11-12: pH value in tenths (little endian, divide by 10 for actual pH)

---

### 13. Chlorinator ORP Setpoint

Target ORP (oxidation-reduction potential) level.

**Pattern:** `02 00 90 FF FF 80 00 1D 0F 3C` (followed by register)

**Example:**

```
02 00 90 FF FF 80 00 1D 0F 3C 02 8A 02 8E 03
                              ^^ ORP setpoint register
                                 ^^ ^^ ORP value in mV (little endian)
                                       650 mV (0x028A)
```

**Data Fields:**

- Byte 10: ORP setpoint register (0x02)
- Bytes 11-12: ORP value in millivolts (little endian)

---

### 14. Chlorinator ORP Reading

Current ORP reading from the sensor.

**Pattern:** `02 00 90 FF FF 80 00` (followed by subcommand)

**Example:**

```
02 00 90 FF FF 80 00 1F 0F 3E 02 0A 02 0E 03
                              ^^ ORP reading register
                                 ^^ ^^ ORP value in mV (little endian)
                                          522 mV (0x020A)
```

**Data Fields:**

- Byte 10: ORP reading register
- Bytes 11-12: ORP value in millivolts (little endian)

---

### 15. Internet Gateway Serial Number

Serial number of the internet gateway module.

**Pattern:** `02 00 F0 FF FF 80 00 37 11`

**Example:**

```
02 00 F0 FF FF 80 00 37 11 B8 04 A3 15 21 00 DD 03
                              ^^ Unknown
                                 ^^ ^^ ^^ ^^ Serial number (little endian)
                                               0x002115A3 = 2168227
```

**Data Fields:**

- Byte 10 
- Bytes 11-14: Serial number (32-bit little endian)

---

### 16. Internet Gateway IP Address

IP address and signal strength of the gateway.

**Pattern:** `02 00 F0 FF FF 80 00 37 15 BC`

**Example:**

On Startup (no connection)
```
02 00 F0 FF FF 80 00 37 15 BC 01 01 01 03 00 00 00 00 00 06 03
```

With IP address (wifi connected)
```
02 00 F0 FF FF 80 00 37 15 BC 01 01 01 07 C0 A8 00 17 2B B4 03
                              ^^ Unknown
                                 ^^ Unknown
                                    ^^ Unknown
                                       ^^ Unknown
                                          ^^ ^^ ^^ ^^ IP address (192.168.1.23)
                                                      ^^ Signal level (43)
```

**Data Fields:**

- Byte 10: Unknown
- Byte 11: Unknown
- Byte 12: Unknown
- Byte 13: Unknown
- Bytes 14-17: IP address (4 bytes, standard order)
- Byte 18: WiFi signal level (0-100)

---

### 17. Internet Gateway Communications Status

Status of the gateway's internet connection.

**Pattern:** `02 00 F0 FF FF 80 00 37 0F B6`

**Example - Communicating with server:**

```
02 00 F0 FF FF 80 00 37 0F B6 02 01 80 83 03
                              ^^ Unknown 
                                 ^^ ^^ Status code (little endian)
                                          0x8001 = 32769: Communicating with server
```

**Data Fields:**
- Byte 10: Unknown (observed as always 0x02)
- Bytes 11-12: Communications status code (little endian)

**Status Codes:**

- `0x0000`:`0` Unknown
- `0x0400`:`1024` Unknown
- `0x0401`:`1025` Unknown
- `0x8000`:`32768` Unknown
- `0x8001`:`32769` Communicating with server
- `0xF002`:`61442` Unknown

---

## Implementation Notes

### Message Validation

All messages should be validated before processing:

1. **Start byte:** Must be `0x02`
2. **End byte:** Must be `0x03`
3. **Minimum length:** At least 13 bytes for checksum verification
4. **Checksum:** Calculate and compare with received checksum byte

### Thread Safety

When implementing a decoder:

- Protect shared state with mutexes/semaphores
- Use snapshots for publishing to avoid holding locks during I/O
- Validate all array indices before access

### UART Configuration

The Astral bus uses:

- **Baud rate:** 9600
- **Data bits:** 8
- **Parity:** None
- **Stop bits:** 1
- **TX inversion:** May be required depending on interface hardware

## Example: Complete Message Decode

```
02 00 50 FF FF 80 00 14 0D F1 01 01 03
^^ Start byte
   ^^^^^  Source: 0x0050 (Controller)
         ^^^^^  Destination: 0xFFFF (Broadcast)
               ^^^^^  Control: 0x8000
                     ^^^^^^^^  Command: Mode message pattern
                              ^^ Data: 0x01 = Pool mode
                                 ^^ Checksum: 0x01 (sum of byte 10)
                                    ^^ End byte
```

**Decoded:** Controller broadcasts Pool mode to all devices.
