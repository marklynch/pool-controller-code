Version 1.1.0
# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).


## [TODO]
Don't publish to MQTT after every change - add small debounce to it

## [Unreleased]

### Added
- Added `/status` endpoint which shows current state as json page (needs testing).
- Added initial MQTT config for home assistant configuration.
- Added `/mqtt_config` endpoint to configure mqtt.
- Added navigation to the html pages and made use common headers
### Changed
- Pulled out led functions to helper file.
- Pulled out web handlers to own files.
- Pulled out pool state to own file.

### Fixed
- Wifi page now deduplicates AP's and shows current network selected
- Log out the IP address correctly when it has one.

### Removed

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