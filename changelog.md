Version 1.1.0
# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Decoding of the following messages types:
- Mode (Pool, Spa)
- Lighting Zones (Off,Auto,On)
- Temperature - Spa set temp, pool set temp, current temp
- Chlorinator, pH etpoint, pH reading, ORP setpoint, ORP reading.
- Temperature Scale, Celcius or Fahrenheit


## [0.0.1] - 2026-01-14

### Added
- Initial commit of code that can listen on the bus and output the bytes it reads
- log to the monitor console
- log to a tcp connection on port 7373