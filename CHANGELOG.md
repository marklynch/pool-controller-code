Version 1.1.0
# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).


## [TODO]
- Sometimes messages fail - likely clashing message on bus. Make them do a retry
- Solar is not yet supported
- Add basic controls without home assistant.
- Optimise for homekit via Homekit Bridge

## [Unreleased]
### Added
- Added a browser-based install page published to GitHub Pages (`site/index.html`, deployed by `.github/workflows/pages.yml`), letting someone flash a board from desktop Chrome or Edge over Web Serial without installing ESP-IDF or esptool: the page uses ESP Web Tools to write the existing `pool-controller-full-*.bin` merged image at offset 0, and shows the release version, wiring/cable prerequisites and the post-flash WiFi provisioning steps. The page, the pinned `esp-web-tools` bundle (vendored from npm at deploy time rather than loaded from a CDN, so there is no runtime CDN dependency and the flashing code that ships is reviewable) and the firmware binary are all served from the same origin, so the browser's fetch of the binary is same-origin and does not depend on the release CDN's CORS headers. The deploy is invoked as a job from `build.yml` after a release is published — not off the `release: published` event, which never fires for releases published with `GITHUB_TOKEN` — and also runs on pushes touching `site/` and on manual dispatch. Requires Settings → Pages → Source to be set to "GitHub Actions" once
### Changed
### Removed
### Fixed
### Deprecated
### Security

## [1.11.0] - 2026-07-21
### Added
- Added a device reboot control: a Reboot button on the firmware-update web page (with confirmation and auto-reload countdown) backed by a new `/reboot` endpoint, and a matching Home Assistant Reboot button via MQTT (`pool/<device_id>/reboot`)
- Decoded the solar protocol messages, previously logged as unhandled: register 0x3A (slot 0x01) is the Solar Setpoint in °C (confirmed by changing the setpoint in the UI and watching the rebroadcast; polled by the Internet Gateway every cycle), CMD 0x2D is the Solar Setpoint Broadcast the touchscreen fires when the setpoint is changed (1-byte °C), and CMD 0x2C is the Solar Status Broadcast carrying the solar mode (Off/Auto/On), a config bitmask (Winter/Summer season, Flush daily, Filter pump required for solar), the temperature differential, and two bytes read as the pool-water/roof temperature readings (byte order unconfirmed) — all three handled log-only and documented in PROTOCOL.md, with the remaining 0x2C unknowns (a byte tracking differential − 3, an always-set bit) flagged for further captures
- Decoded register 0x30 (slot 0x01) as the Current Water Temperature mirror, previously logged as unhandled: the touchscreen mirrors the controller's CMD 0x16 water-temperature reading into this register (same 2-byte {temp1, temp2} payload) for the Internet Gateway to poll each cycle, confirmed by the value tracking the controller's own sensor across captures — including through a temperature change, and staying with the controller's reading when the heat pump's differed — and by installs without a controller temp sensor never answering the poll; handled log-only since CMD 0x16 remains the authoritative state source, and documented in PROTOCOL.md along with a still-unknown register 0x30 slot 0x03 (1-byte, only 0x00 observed) left unhandled so novel values keep surfacing
- Added a per-zone Resync button to Home Assistant for multicolor light zones: pressing it broadcasts the Light Resync Command (CMD 0x3C) for that zone, impersonating the Touchscreen (the only observed sender); the button is created alongside the zone's color effect list — so only multicolor-capable zones on installs with a known light type get one — and is removed if the zone stops being multicolor
### Changed
- Renamed CMD 0x3C from "Light Refresh Command" to "Light Resync Command" in PROTOCOL.md and the decoder to better describe its effect of resynchronizing a zone's light

## [1.10.0] - 2026-07-19
### Added
- Decoded CMD 0x3C as the Light Refresh Command, previously logged as unhandled: a touchscreen broadcast carrying a light zone index that refreshes/resyncs that zone's light, observed during light configuration sessions and color operations — handled log-only with out-of-range zone indexes recorded as "undocumented" entries on the Unknown Messages page; documented in PROTOCOL.md with the exact effect at the light hardware flagged unconfirmed
- Added light color control to the Home Assistant light entities for multicolor zones: each zone whose light is multicolor-capable (and whose system light type is known via register 0xF0) now exposes the model's color list — the SLX or Delta subset of the shared color table — as the light's effect list, with the current color shown as the active effect and color picks sent to the controller as a Gateway-sourced register write; light entity discovery re-publishes automatically if the light type or a zone's multicolor capability changes, and the MQTT light state now uses the canonical color names (e.g. "User 1" instead of "User1") so the reported color matches the effect list
- Decoded register 0xF0 (slot 0x01) as the system-wide Multicolor Light Type selection, previously an unknown register: the light model chosen in the touchscreen's light setup (0x00 = SLX, 0x01 = Delta, 0xFF = no multicolor light), confirmed by switching the type in the UI and watching the register rebroadcast. Surfaced as a "Multicolor Light Type" row on the status page and a `multicolor_light_type` field in the `/status` JSON ("None"/"SLX"/"Delta"); an unmapped model index is shown as "Unknown (0xXX)" and recorded as an "undocumented" entry on the Unknown Messages page — documented in PROTOCOL.md, where the selected model also determines which subset of the shared light color code table applies — the full SLX and Delta color code subsets were mapped by cycling every color on each type, and a Gateway-sourced 0x3A color write was verified working (sets the light and updates the touchscreen)
- Decoded CMD 0x07 as the Lighting Zone Color Broadcast, previously logged as unhandled every ~10 s on installs with a multicolor light: the color companion to the CMD 0x06 light config broadcast ({zone_idx, color} payload), emitted only for multicolor-capable zones, with the color byte mirroring the zone's Light Zone Color register (0xD0+zone, slot 0x01) — handled log-only since the register broadcast remains the state source, with out-of-range zone indexes recorded as "undocumented" entries on the Unknown Messages page; documented in PROTOCOL.md with the color enumeration flagged install-specific
- Decoded the Light Zone Enabled registers (0x90–0x93, slot 0x01), previously logged as unhandled: a 1-byte per-zone flag reporting whether each light zone is configured in the controller (0x01 = configured, 0x00 = not), completing the slot-0x01 per-zone register family alongside multicolor/name/state/color/active. Configured zones are rebroadcast regularly while unconfigured ones only appear at startup or after a configuration change; the flag now drives the zone's configured state (a newly seen zone wakes the register requester to poll its name/color, and an unconfigured zone stops publishing to Home Assistant) — documented in PROTOCOL.md

## [1.9.0] - 2026-07-17
### Added
- Decoded the CMD 0x2B controller heartbeat — a ~60 s unicast from the Connect 10 controller (0x0062) directly to the Touchscreen (0x0050), payload `02 00` in every capture, previously logged as an unhandled frame each cycle. Now handled log-only by `handle_controller_heartbeat`, which records any deviation from `02 00` as an "undocumented" entry on the Unknown Messages page. As one of only two message types the controller sends straight to the touchscreen, it is a prime candidate carrier for a controller-originated state signal such as service mode (documented in PROTOCOL.md)
- Decoded Service Mode: byte 11 of the Connect 10 controller's CMD 0x12 status is a bitfield (bit 0 = heater on/off, bit 1 = service mode), confirmed by a service-mode capture — documented in PROTOCOL.md and surfaced as a new Home Assistant "Service Mode" binary sensor (published on first status frame) and a `service_mode` field in the `/status` JSON
- Decoded the Genus Heater's (Active i25 Evo heat pump, 0x0070) CMD 0x12 device status, previously logged as unhandled: same 4-byte payload shape as the gas heaters with matching heater-on and water-flow bits, but a heat-pump-specific upper-bit set (0x13 observed immediately after a Gateway heater-on command, so bit 4 is read as actively heating rather than the gas heaters' lockout); heater on/off feeds the Heater 1 state in Home Assistant, and any status value outside the observed set — or a non-zero padding byte — is recorded as an "undocumented" entry on the Unknown Messages page — documented in PROTOCOL.md with the tentative bits flagged unconfirmed
### Changed
- Extended undocumented-payload flagging (`record_undocumented()`) across more decoders as protocol-research groundwork toward service-mode detection, so a novel byte value is surfaced on the Unknown Messages page instead of being silently absorbed: CMD 0x26 Configuration (reserved bits 5–7 set, or the documented always-1 bit 0 clear), the Connect 10 controller's own CMD 0x12 status (padding byte 10, state byte 11, and the unknown byte 12 — a likely service/interlock carrier), CMD 0x05 touchscreen ack, CMD 0x37 gateway comms status (codes absent from the known table), the VX 11S v3 chlorine-output byte 12 (to catch a LOW SALT / NO FLOW warning capture), CMD 0x06 light config (out-of-range zone index / undocumented status byte), the CMD 0x27 short-form valve data byte, CMD 0x38 timer day-bitmask bit 7, and the favourite-enable register — documented in PROTOCOL.md
### Fixed
- Fixed register writes made from the Viron Chlorinator's app (light zone on/off and color picks) being logged as unhandled: the chlorinator broadcasts the same CMD 0x3A Register Write the Gateway uses, so the command is now dispatched on its CMD byte regardless of source (handler renamed to `handle_register_write_request`), with the newly observed light-zone-color write target (0xD0–0xD7/slot 0x01) and its app color codes documented in PROTOCOL.md — flagged as an install-specific enumeration since the codes conflict with the reference system's color values
- Fixed channel toggles made at the Connect 10 controller's physical buttons being logged as unhandled: the controller broadcasts the same CMD 0x10 Channel Toggle Command the Gateway uses for remote toggles (same 1-byte channel-index payload), so the command is now dispatched on its CMD byte regardless of source — controller-sourced examples (channel indexes 0x04–0x07) documented in PROTOCOL.md
- Fixed favourite activations made at the Connect 10 controller itself (e.g. All Auto) being logged as unhandled: the controller unicasts the same CMD 0x2A Favourite Control Command to the Touchscreen as the Gateway does for remote activations, so the command is now dispatched on its CMD byte regardless of source and updates the active favourite in Home Assistant — both sources documented in PROTOCOL.md
- Fixed CMD 0x26 Configuration broadcasts being counted as unknown/unhandled on every cycle: the handler returned `false` unconditionally, routing each normal config frame into the Unknown Messages buffer. It now decodes the frame, returns decoded, and records only genuine anomalies (see above), so the unknown-message counter and page reflect real unknowns
- Fixed the inbuilt heater wrongly reporting On while the controller is in service mode with the heater off: the CMD 0x12 status byte was treated as a boolean (any non-zero value = On), so the service-mode value 0x02 read as heater on; the heater state now comes from bit 0 only


## [1.8.2] - 2026-07-16
### Changed
- Decoded the newly discovered CMD 0x15 Mode Set Command: broadcast from the Touchscreen address (0x0050), switches Spa/Pool mode using the same encoding as the 0x14 status (Spa=0x00, Pool=0x01, unlike the inverted 0x2A values), confirmed by injection testing — documented in PROTOCOL.md and decoded into pool state, so mode switches commanded by other senders update Home Assistant immediately
- The Home Assistant Spa/Pool mode select (mode/set) now switches mode with the dedicated CMD 0x15 Mode Set Command instead of activating the Pool/Spa built-in favourites via CMD 0x2A; mode values throughout the code now use the new MODE_SPA/MODE_POOL constants
- Renamed CMD 0x2A from "Mode/Favourite Control Command" to "Favourite Control Command" throughout PROTOCOL.md and the code (handler, pattern constant, comments and logs), since its value space is the favourite slots; the Spa/Pool MQTT mode select is unaffected
### Fixed
- Fixed the Home Assistant "Firmware" update entity reverting to "Update available" (with a live Install button) during the post-flash reboot: the rebooting window now reports as still installing with an indeterminate progress bar, flipping to "Up-to-date" once the new firmware boots and reports its version

## [1.8.1] - 2026-07-16
### Fixed
- Added a logo to the Home Assistant "Firmware" update entity (shown on the Updates page and in the update dialog) by pointing its `entity_picture` at the project favicon hosted on GitHub
- Fixed phantom "Unused" / "Unused Active" Home Assistant entities appearing for unconfigured channels: a channel-state register (0x8C–0x93), which the controller broadcasts for unused channels too, wrongly marked the channel as configured and published discovery for it under the fallback type name "Unused"; a channel is now only marked in use once its type is known

## [1.8.0] - 2026-07-16
### Added
- GitHub firmware auto-update: the device now periodically checks the project's recent GitHub releases (every 12 h, plus shortly after boot) — the latest plus the previous 4 versions, read from the streamed `releases.atom` feed — and can install any of them over-the-air by pulling that release's `pool-controller-update-<tag>.bin` asset directly over HTTPS (`esp_https_ota`), no local file handling needed. The `/update` page gained an "Update from GitHub" section showing whether a newer release is available (with a release-notes link) and a version dropdown to install or roll back to a specific release, with live download progress. A new Home Assistant `update` entity ("Firmware") mirrors the check and installs the latest via the HA *Install* button, reporting installed/latest versions and download progress. Repo, number of versions tracked, check interval, and timeouts are configurable via `FW_UPDATE_*` in `config.h`. (New `firmware_update` module; see OTA_UPDATE.md.)
- WiFi mesh roaming support: on connect the device now scans all channels and joins the strongest AP broadcasting the SSID (instead of the first one found), and advertises 802.11k/v so mesh networks like eero can steer it to a better node while connected
- Decoded the Channel Category registers (0xF5–0xFC, slot 0x01, one per channel: Pool equipment / Light / Controlled Heater Power; only broadcast for in-use channels) — documented in PROTOCOL.md Appendix A and now decoded into pool state instead of being logged as unhandled registers
- Decoded the Active Favourite register (0x20, slot 0x03): reports the currently active favourite/mode using the CMD 0x2A values, with 0xFF = none active (All Off reports as Pool and All Auto as none, since the controller treats them as momentary actions rather than states) — documented in PROTOCOL.md Appendix A and fed into the Home Assistant favourite select, which gains a status-only "No Favourite" option; favourites activated outside HA now update the select (immediately for gateway/MQTT commands, at the next ~8-minute register dump for touchscreen activations)

## [1.7.0] - 2026-07-14
### Added
- Saving of unknown bus messages, added web UI to view them
- Periodic heap-stats logging (free, minimum-free watermark, largest free block) every 5 minutes via a new low-priority `heap_monitor` task, so a slow memory leak or growing fragmentation is visible as a trend in the console/TCP log history before it can exhaust the heap. The home page System table now also shows a Memory row (free / min-free), using the `memory` fields already present in the `/status` JSON.
### Changed
- Note about CMD 0x05 observed with payload 0x00. Added to PROTOCOL.md and new sample trace.
- Note about CMD 0x12 observed with payload 0x01 0x00. Added to PROTOCOL.md and new sample trace.
- Parsing for CMD 0x12 updated so `0x01 0x00` no longer reported as unexpected.
- Full decoding of CMD 0x12 status byte for Gas Heaters (HiNRG `0x0072` and ICI (`0x0074`))
- Rewrote frame level parser to use a sliding window for better resync on errors
- Track frame resync events per failure type (no-start, header/data checksum, control, length, end-byte, overflow) and show the breakdown on the status page, separate from message-level decode errors
- Unknown Messages page now tags each captured frame with its specific reason (no start, overflow, header/data checksum, length, bad framing) instead of a generic "error" chip
- Handlers can now flag recognised frames carrying an undocumented field value via the new `UNKNOWN_REASON_UNDOCUMENTED_PAYLOAD` reason (`record_undocumented()`): the known fields are still decoded/published and the frame is counted as decoded, while the raw frame is surfaced on the Unknown Messages page as a non-error "undocumented" chip (amber, distinct from red framing errors). Applied across the decoders wherever a field value falls outside its documented set: heater 1/2 state (0xE6/0xE9), gas-heater status (0x12), Spa/Pool mode (0x14), touchscreen status (0x12), chlorinator mode (0x12) and pump-mode (0x0F), VX 11S status (0x12), pump speed command (0x18) and buttons (0x1B), mode/favourite command (0x2A), gateway register writes (0x3A), and the register-based channel type/state, light zone state/colour/name, valve state, and channel-status broadcasts (0x0B/0x27/0x38).
- **Breaking (HTTP):** `/status` JSON's `message_counts.errors` (per-type protocol error counters) and `message_counts.error_detail` (introduced in 1.6.0-rc1) are removed, replaced by the new per-type `resyncs` object (`total`, `no_start`, `header_checksum`, `bad_control`, `bad_length`, `bad_end`, `data_checksum`, `buffer_overflow`) emitted by the sliding-window frame parser. Any external consumer reading the old fields needs to switch to `resyncs`.
- Frame parser and decoder now also accept "discovery" packets (control bytes `0x00 0x00`): a shorter 11-byte frame shape with no payload and no data-checksum byte, alongside the existing `0x80 0x00` data packets
### Fixed
- Fixed the home page Temperature row showing nothing useful since the per-source temperature refactor: the page's script still read the removed `/status` field `temperature.current`; it now renders one row per temperature-reporting device (e.g. "Genus Heater Temperature", "Connect 8/10 Temperature 1/2") from the per-device `temperature1`/`temperature2` fields
- TCP bridge no longer hangs when a connected client stalls or dies silently. The client socket is now non-blocking with a keep/drop policy on every send (full send keeps the client; a full send buffer drops the message but keeps the connection; a partial write or hard error drops the client), so a stuck peer can never wedge the bridge task — which is also the only task reading the UART, meaning such a wedge previously froze the whole device with no logs and no recovery. Added TCP keepalive (idle 30s, interval 5s, count 3) so a silently dead peer is detected and the single client slot freed.
- MQTT client is no longer stopped from inside the WiFi event handler on disconnect. `esp_mqtt_client_stop()` blocks waiting for the MQTT task, and calling it from the system event-loop task could stall all further WiFi/IP event processing if the MQTT task was itself stuck on a dead socket during the same outage. The client is now started once on first connectivity and left to esp-mqtt's built-in reconnection, which handles WiFi drops/restores on its own. `mqtt_client_start()` is now idempotent so repeated reconnects are no-ops.
### Security
- Fixed a DOM-based XSS on the home page: channel/zone/valve names read from the bus were injected into the pool status table via `innerHTML`. They are now set with `textContent` so bus-derived strings can never be parsed as HTML/script.

## [1.6.0] - 2026-06-16
### Changed
- Combined the HiNRG and ICI gas-heater status handlers (CMD 0x12) into a single handler matched by either device pattern
### Fixed
- Fixed HiNRG Gas Heater (0x0072) status messages being logged as unhandled: the CMD 0x12 match pattern used the wrong header checksum byte (0x15 instead of 0x14), so the heater's device-side status was never decoded

## [1.6.0-rc1] - 2026-06-10
### Added
- Global protocol-error counters alongside the decoded/unknown totals, broken down by failure type (no start byte, bad control bytes, no end found/buffer overflow, bad framing, length-field mismatch, header checksum, data checksum); exposed in the `/status` JSON (`message_counts.errors` and `message_counts.error_detail`) and the home page Messages row
- Decode Heater 2 pool setpoint (register `0xEA`) in both the touchscreen register broadcast and gateway register-write paths, instead of logging it as an unknown register
- Updated CMD 0x17 to show its payload is Spa and Pool temperature setpoints (see issue #31)
- Added HiNRG Gas Heater (0x0072) support (see issue #31)
- Added per-heater pool and spa temperature setpoints, exposed as separate Home Assistant Number entities per heater ("Heater N Pool/Spa Setpoint"); writable via CMD `0x19` for Heater 1 and register writes `0xEA`/`0xEB` for Heater 2
- Added Heater 2 on/off support: state decoded from register `0xE9` and controllable via the "Heater 2" switch
- VX 11S v3 chlorinator status handler
### Changed
- Combined documentation for heater devices sending cmd 0x16 (water temperature)
- Combined existing heater setpoint temperature handlers
- **Breaking (MQTT/HTTP):** replaced the single global "Pool Setpoint"/"Spa Setpoint" entities with per-heater setpoints. The `pool/<id>/setpoints/state` topic and the global setpoint Number entities are gone, replaced by `pool/<id>/heater/<index>/setpoints/state` and per-heater entities; `GET /status` now carries setpoints inside each `heaters[]` entry instead of a global `temperature` block
- Heater setpoints are now keyed per heater by the controller's registers (`0xE7`/`0xE8` Heater 1, `0xEA`/`0xEB` Heater 2); the physical heater devices' CMD `0x17` broadcasts are now log-only so an unplumbed heater's `0x0A` default no longer clobbers the active setpoint
- Introduce reg_id_t enum for named register IDs
### Fixed
- Configured devcontainer to use ESP-IDF 5.5 for consistency
- Fixed pool/spa setpoint commands having no effect after the per-heater refactor: the device now subscribes to the new `heater/<n>/pool_setpoint/set` and `heater/<n>/spa_setpoint/set` command topics (the old `temperature/...` subscriptions pointed at handlers that no longer exist)
- Fixed phantom "Chlorine Output Level", "Pump Speed" and pH/ORP Home Assistant entities appearing on systems without a chlorinator or variable-speed pump: chemistry and pump discovery is now published lazily on each entity's first valid value instead of unconditionally at MQTT connect, and pH/ORP setpoints report `null` until a real setpoint has been received

## [1.5.1] - 2026-05-19
### Changed
- Increase the timeout and retry count if wifi is not available to 15 second retry and 10 attempts. [#25](https://github.com/marklynch/pool-controller-code/issues/25)

## [1.5.0] - 2026-05-19
### Added
- Added tests runner for message decoder to make it easier to track changes and prevent regressions
- Added GitHub Actions workflow that runs the host-based test suite on every push and pull request
- Added many new unknown registers to PROTOCOL.md
- Added Viron XT Variable Speed Pump support (device `0x00A0`): decodes speed telemetry (CMD `0x3B`)
- Added pump physical speed preset button presses (CMD `0x1B`, mapping Low/Med/High), and controller-to-pump speed commands (CMD `0x18`, mapping Low/Med/High)
### Changed
- Renamed device `0x00A0` from "Internal Salt Cell" to "Viron XT Pump"
- Renamed CMD `0x18` from "Chlorinator Cell Mode" to "Pump Speed Command" in the command name table
- Updated temperature samples by @lawther
### Fixed
- Fixed mixed-validity temperature logging (CMD `0x16` / `0x31`): when one of `temp1` / `temp2` is INVALID the still-valid temperature now logs its decoded °C value rather than a generic `OK (raw 0xXX)` label
- Re-enabled the `test_message_decoder` and `test_mqtt_commands` host-test suites that had drifted out of sync with the current decoder/state and `bus_send_bytes`/`s_pool_state` interfaces, and cleared the `SKIP_LIST` in `run_tests.sh`
- Fixed: ICI Gas Heater setpoints not being saved to pool state by @lawther

## [1.4.1] - 2026-05-19
### Added
- Add support for ICI Gas Heater (0x0074)

## [1.4.0] - 2026-05-19
### Added
- Per-source temperature storage: `temp1`, `temp2`, `temp1_valid`, `temp2_valid` and `single_sensor_source` are now stored directly on each `seen_device_t` entry. Multiple temperature sources (Connect 8/10 + Genus Heater variants) are kept distinct rather than overwriting a single global field.
- HTTP `/status`: each device entry in the `devices[]` array now carries `temperature1` (and `temperature2` for multi-sensor sources like the Connect 8/10). Devices that don't broadcast CMD `0x16` get no temperature fields at all.
- New per-source MQTT temperature entities, discovered lazily on first reading from each `(source, sensor)` pair. Friendly names: "Temp - Genus Heater" for single-sensor sources, "Temp 1 - Connect 8/10" / "Temp 2 - Connect 8/10" for multi-sensor. Entity IDs namespaced by controller, e.g. `sensor.pool_controller_<mac>_connect_8_10_temp_1`.
- Invalid-temperature sentinel: water temperature readings (CMD `0x16` and `0x31`) with a raw value `>= 0xA0` (160°C) are treated as a disconnected sensor — logged as a warning and skipped instead of being stored or published.
- Picked up the previously-unhandled `0x0072` Genus Heater variant of CMD `0x16` (same single-byte layout as the `0x0070` Genus Heater).
### Changed
- Pulled out the water temperature reading (CMD `0x16`) to be source-agnostic — single unified `handle_temp_reading` dispatched on the CMD byte and routed by payload length: 2-byte `{temp1, temp2}` from `0x0062` Connect 8/10 (LEN `0x0E`); 1-byte `{temp1}` from `0x0070`/`0x0072` Genus Heater family (LEN `0x0D`). Removes the `MSG_TYPE_TEMP_READING` and `MSG_TYPE_GENUS_HEATER_TEMP_READING` patterns and the dedicated `handle_genus_heater_temp_reading`.
- Merged CMD `0x31` (Water Temperature Reading alt) into the unified `handle_temp_reading()` — `0x16` and `0x31` share the same `{temp1, temp2}` field layout. CMD `0x16` is the canonical source (writes onto the device entry and publishes MQTT); CMD `0x31` is log-only to avoid dual MQTT updates for the same reading. Removes the `MSG_TYPE_TEMP_READING2` pattern and the dedicated `handle_temp_reading2`.
- MQTT topic split: setpoints (`pool_sp`, `spa_sp`, `scale`) moved from `pool/<id>/temperature/state` to a dedicated `pool/<id>/setpoints/state`. Per-sensor temperature readings publish to `pool/<id>/temperature/<slug>/<index>/state` (multi-sensor) or `pool/<id>/temperature/<slug>/state` (single-sensor), where `<slug>` is derived from `get_device_name()` (e.g. `connect_8_10`, `genus_heater`). **Breaking change for MQTT consumers**: the old `pool/<id>/temperature/state` topic is gone; any HA automation bound to the old "Temperature" sensor entity will need to rebind to the new per-source entities.
- HA discovery for setpoint number entities (`Pool Setpoint`, `Spa Setpoint`) updated to read from the new `setpoints/state` topic.
### Removed
- `pool_state.current_temp` and `pool_state.temp_valid` — temperatures now live per-source on `seen_device_t`. Consumers (MQTT publish, HTTP status, HA discovery) migrated to the per-device fields.
- Global "Temperature" HA sensor entity (`publish_temperature_discovery`) — replaced by per-source entities discovered lazily on first CMD `0x16` reading.
- `HTTP /status` field `temperature.current` — replaced by `temperature1`/`temperature2` on each device entry.
### Fixed
- HA MQTT discovery entity IDs: switched from the unrecognised `default_entity_id` field to `object_id` across all entity types (temperatures, setpoints, pH/ORP, heaters, channels, lights, valves, mode, favourites), so HA now derives stable entity IDs like `sensor.pool_controller_<mac>_orp` instead of falling back to `sensor.unnamed_device_<n>`
- CMD `0x16` (Connect 8/10 variant): byte 11 was previously labelled "unknown — always 0x00", now decoded as a second water temperature (`temp2`).
- CMD `0x31` byte 11: was previously labelled "unknown — always 0xA6 in observed samples", now decoded as the same `temp2` field as CMD `0x16` byte 11. The `>= 0xA0` values (`0xA6`, `0xAD`, `0xAF` observed) are the disconnected-sensor sentinel — confirmed by paired captures where CMD `0x16` byte 11 reads `0x00` in the same cycle.

## [1.3.1] - 2026-05-18
### Changed
- Pulled out the CMD 0x38 and 0x39 to be handled as commands rather than patterns as other devices send these now.
- Pulled out the temperature setpoint command (CMD 0x19) to be source agnostic as the Genus Heater also sends this.
- Consolidated the chlorinator pH/ORP setpoint (CMD `0x1D`) and reading (CMD `0x1F`) dispatch into source-agnostic CMD-byte routing in `dispatch_message`, so the existing four `handle_chlor_{ph,orp}_{setpoint,reading}` handlers now fire for both `0x0090` RolaChem and `0x0084` Viron sources — payload shape `{channel, value_lo, value_hi}` is identical across the two variants, only the source address (and resulting checksum1 byte) differed. Removed the `MSG_TYPE_CHLOR` prefix pattern and the four `CHLOR_*_{SETPOINT,READING}` sub-type pattern constants

## [1.3.0] - 2026-05-18
### Added
- Added handler and PROTOCOL.md §32 entry for the Chlorinator Status Broadcast (CMD `0x12` from both `0x0090` and `0x0084` variants) — 1-byte mode payload, tentatively mapped to the standard `0x00`=Off / `0x01`=Auto / `0x02`=On channel-state convention (marked ⚠️ pending an Off↔Auto↔On transition capture); `handle_chlor_status` logs the resolved mode name and populates new `pool_state->chlor_mode` / `chlor_mode_valid` fields. Updates the §Known Command Bytes `0x12` shared-device table to list five devices and the Chlorinator broadcasts table to include `0x12`
- Added handler and PROTOCOL.md §33 entry for the Chlorinator Firmware Version (CMD `0x0A` from `0x0084`) — structurally identical to the §17 Gateway and §21 Touchscreen firmware-version messages with a 2-byte `{major, minor}` payload; `handle_chlor_version` populates new `pool_state->chlor_version_major` / `_minor` / `_valid` fields and logs the version. Updates the §Known Command Bytes `0x0A` shared-device table to list three devices and the Chlorinator broadcasts table to include `0x0A`
- Added PROTOCOL.md §34 entry for the Temp Sensor Firmware Version (CMD `0x0A` from `0x0062`) — same `{major, minor}` payload shape as the other firmware-version broadcasts; observed sample is v2.6 (`02 06 08`). Populates new `pool_state->temp_sensor_version_*` fields via the consolidated firmware-version handler. Updates the §Known Command Bytes `0x0A` shared-device table to list five devices (now also covering `0x0070` Heatpump explicitly) and the Temperature sensor broadcasts table to include `0x0A`
- Added a `devices` section to the `/status` JSON listing every source address observed on the bus, with resolved name, firmware version (when a CMD `0x0A` has been seen), and per-device `message_counts.decoded` / `unknown` counters — backed by a new `pool_state->seen_devices[]` registry populated in `decode_message`; broadcast (`0xFFFF`) is excluded from the registry
### Changed
- Changed `get_device_name()` to format a self-describing `Unknown 0xHHLL` fallback into a caller-supplied buffer instead of returning NULL, removing the four-branch NULL-handling block in `decode_message()`
- Moved the global decoded/unknown message counters from `tcp_bridge.c` into `pool_state_t` (`messages_decoded_total` / `messages_unknown_total`) and unified global + per-device counter updates in a single mutex-held block at the end of `decode_message`; `tcp_bridge_get_decoded_count()` / `_unknown_count()` and the result-branching at the decode call site removed. Side effect: frames that fail the very first sanity checks (`len < 12`, missing `0x02`/`0x03` framing) no longer count as "unknown" — counts now reflect protocol-level decoded-vs-unknown only, not frame-integrity glitches
- Consolidated the four per-device firmware-version handlers (`handle_touchscreen_version`, `handle_heatpump_version`, `handle_gateway_version`, `handle_chlor_version`) into a single source-agnostic `handle_firmware_version` dispatched on `data[7] == 0x0A` regardless of source — the payload layout (`{major, minor}`) is identical across all five known sources, so per-source patterns and handlers were redundant. The handler switches on source address to populate the appropriate `pool_state->*_version_*` field (touchscreen/temp-sensor/chlorinator/gateway have dedicated fields; heatpump and any future sources are log-only). Removed the four `MSG_TYPE_*_VERSION` pattern constants and the four scattered dispatch entries
- Replaced the eight per-grouping shared-CMD tables in PROTOCOL.md §Known Command Bytes (Register protocol, Device status 0x12, Firmware version 0x0A, Temperature reading 0x16, Temperature setpoint 0x17, Chlorinator cell mode 0x18, plus per-source broadcast tables for touchscreen/temp-sensor/chlorinator/gateway) with a single master "Known Commands" table — one row per CMD byte, with `Direction`, `Variants / Notes`, `Section(s)`, and a new `In code?` column that distinguishes handlers actually implemented in `message_decoder.c` from CMDs that are documented only. Surfaces four doc-vs-code gaps (`0x0F`, `0x18`, `0x25`, `0x28`) at a glance. First step in a planned restructure toward a command-centric doc
- Consolidated the four per-device firmware-version sections in PROTOCOL.md (§17 Internet Gateway, §21 Touchscreen, §33 Chlorinator, §34 Temp Sensor) into a single generic §17 "Firmware Version" with a unified Known Sources table covering all five observed sources (Touchscreen `0x0050`, inbuilt heater `0x0062`, Heatpump `0x0070`, Chlorinator `0x0084`, Gateway `0x00F0`). §21, §33, and §34 fully removed — TOC entries, Quick Reference rows, and the section bodies themselves — leaving numbering gaps that will resolve when the doc is restructured to a command-centric layout. Master CMD table's `0x0A` row now points only to §17. Removes ~125 lines of duplicated content
### Fixed
- Corrected device-address labels in `get_device_name()` and across PROTOCOL.md to reflect new understanding: `0x0062` Temp Sensor → Connect 8/10 Controller (now framed as controller-sourced heater/water-temperature broadcasts rather than messages from an "inbuilt heater"), `0x006F` Controller → Internal Channels (logical destination tag), `0x0070` Heatpump → Genus Heater, `0x0084` Chlorinator → Viron Chlorinator, `0x0090` Chlorinator → RolaChem, `0x00A0` Salt Cell → Internal Salt Cell, `0x00F0` Internet GW → Internet Gateway; renamed `pool_state->temp_sensor_version_*` to `controller_version_*`, `MSG_TYPE_HEATPUMP_*` to `MSG_TYPE_GENUS_HEATER_*`, and `handle_heatpump_*` to `handle_genus_heater_*` to match

## [1.2.1] - 2026-05-17
### Added
- Added register-dispatch entries for Heater 1 state (`0xE6` slot `0x00`) and tentative Heater 2 state (`0xE9` slot `0x00`) so touchscreen CMD `0x38` broadcasts log as `Heater 1/2 state - Off/On` instead of falling through to "Unhandled register"; both are log-only — authoritative Heater 1 state still flows through the CMD `0x12` path that updates `pool_state->heaters[0]`
- Added PROTOCOL.md Appendix A entries for the suspected Heater 2 trio (`0xE9` State, `0xEA` Pool Setpoint, `0xEB` Spa Setpoint) in slot `0x00` — marked tentative (⚠️) based on structural symmetry with the Heater 1 `0xE6`/`0xE7`/`0xE8` trio, a confirmed gateway register-write to `0xEA = 27°C` (CMD `0x3A`), and the matching H2 value in the heater's CMD `0x17` `[H1, H2]` broadcast
### Fixed
- Corrected the §5 Configuration message byte 10 bit 3 interpretation from "heater count (single/two)" to "heater currently active (Off/On)" after direct same-system observation showed bit 3 toggling with heater on/off transitions (heater On → `0x09`, heater Off → `0x01`); `handle_config` now logs `heater=Off/On` and the §5 byte 10 example table is updated accordingly

## [1.2.0] - 2026-05-17
### Added
- Decoded the Internet Gateway Status Broadcast (CMD `0x12` from `0x00F0`) — payload is `{major, minor, embedded_checksum}` where the third byte equals `major + minor`; `handle_gateway_status` now logs the firmware version and validates the embedded checksum, while firmware-version state population remains with §17. PROTOCOL.md §18 promoted from ⚠️ to ✅, with two confirming samples (5.1 → `05 01 06`, 5.0 → `05 00 05`) and a note clarifying that byte 13 is the standard frame checksum, not a data field
### Changed
- Improved unhandled-message logging in `handle_unknown` to include the resolved source/destination device names (via `addr_info`), the CMD byte plus a human-readable name (`get_cmd_name` table covering 0x05–0xFD; CMDs not in the table render as "Unknown CMD 0xXX"), the declared length byte, and the payload section only — making unknown messages triagable from a single log line without duplicating the raw frame already printed as "RX MSG"

## [1.1.0] - 2026-05-16
### Added
- Added device address `0x0070` ("Heatpump", e.g. Active i25 Evo electric heater) to the device-name lookup, plus decoders for its three observed broadcasts: CMD `0x0A` firmware version, CMD `0x16` water-temperature reading (1-byte payload, distinct from the `0x0062` Temp Sensor variant), and CMD `0x17` two-byte `[Heater 1 setpoint, Heater 2 setpoint]` payload
- Added channel type `0xFB` ("Secondary Heater") to the channel-type lookup so channel-status broadcasts decode it by name instead of logging "Unknown (251)"
- Documented the `0x0070` heater variants of CMD `0x16` and `0x17` in `PROTOCOL.md` (§2 and §3) as source-dependent shared command bytes, alongside the existing `0x12`/`0x0A` shared-command tables; added `0x0070` to the device address table and `0xFB` to the §7 channel type list
- Added device address `0x0084` ("Chlorinator 0x84", alternate chlorinator variant mutually exclusive with `0x0090`) and `0x00A0` ("Salt Cell", suspected chlorine generator subordinate to `0x0084`) to the device-name lookup and to the PROTOCOL.md device address table; renamed the `0x0090` lookup string to `"Chlorinator 0x90"` so logs distinguish the two variants
- Added PROTOCOL.md §31 "Chlorinator Cell Mode" documenting the inter-device CMD `0x18` unicast from `0x0084`/`0x0050` to `0x00A0` carrying a 1-byte chlorinator mode (`0x00`=Off, `0x01`=Manual, `0x02`=Auto — tentative pending more captures)
- Documented bits 1, 2 and 3 of the §5 Configuration message byte 10 — bit 1 = mode (`0`=heat, `1`=cooler-only), bit 2 = temperature step (`0`=1°, `1`=2°), bit 3 = heater count (`0`=single, `1`=two heaters) — confirmed by three independent single-bit-toggle captures; `handle_config` now logs all three alongside the existing temperature scale. Corrected the previous (unconfirmed) interpretation of bit 0 as heat/cool — bit 0 was observed to stay `1` across all four samples, so its purpose is now marked unknown

## [1.0.3] - 2026-05-14
### Changed
- Changed log timestamp format from milliseconds since boot (e.g. `(96551957)`) to wall-clock time (`HH:MM:SS.mmm`) — set `CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM=y` in `sdkconfig.defaults`; timestamps show `00:00:xx.xxx` until SNTP syncs after WiFi connects

## [1.0.2] - 2026-05-13
### Added
- Added support for `0x80` (All Off) mode in favourites — handled in `mqtt_commands.c` (command → wire value), `mqtt_publish.c` (wire value → display name), `message_decoder.c` (decoded mode name), and `mqtt_discovery.c` (HA select option); documented in `PROTOCOL.md` alongside `0x81` (All Auto)

## [1.0.1] - 2026-05-12
### Fixed
- Fixed pH sensor MQTT discovery missing `unit_of_measurement` — `publish_ph_discovery` in `mqtt_discovery.c` now includes `"unit_of_measurement": "pH"`, matching the pH Setpoint sensor; without it Home Assistant displayed the pH reading with no unit despite the `ph` device class
- Fixed `mqtt_publish_light`, `mqtt_publish_heater`, `mqtt_publish_mode`, `mqtt_publish_temperature`, `mqtt_publish_favourite`, and `mqtt_publish_chlorinator` publishing state without `retain=true` — after an MQTT reconnect or Home Assistant restart, these entities showed "Unknown" because no retained message existed to restore state; channels and valves were already retained, so this brings all remaining state topics into line

## [1.0.0] - 2026-04-06
### Fixed
- Fixed `register_requester` directly accessing global `s_pool_state` and `s_pool_state_mutex` — `register_requester_start` now accepts `pool_state_t *` and `SemaphoreHandle_t` parameters, matching the dependency-injection pattern used by the message decoder; `main.c` passes `&s_pool_state` and `s_pool_state_mutex` at startup
- Fixed `send_uart_command` in `mqtt_commands.c` bypassing `bus_send_message` — now calls `bus_send_bytes` (extracted from `bus_send_message`) so MQTT commands get TX-wait, TX LED flash, and hex logging consistent with all other bus writes; removed direct `uart_write_bytes` call and `driver/uart.h` include from `mqtt_commands.c`
- Fixed race condition in `dns_server_stop` — replaced unreliable 100ms `vTaskDelay` + conditional `vTaskDelete` with a binary semaphore; the task signals the semaphore on all exit paths before calling `vTaskDelete(NULL)`, and `dns_server_stop` blocks on it (3s timeout) rather than guessing when the task has finished
- Fixed `/status` handler holding the pool state mutex for the entire JSON build — now takes a snapshot immediately after acquiring the mutex and releases it before any cJSON allocation, eliminating contention with the message decoder under load
- Fixed potential silent truncation of MQTT broker URI — increased `broker_uri` static buffer from 192 to 256 bytes in `mqtt_poolclient.c`; the previous margin was tight enough that a max-length broker hostname with port would silently truncate the URI passed to the MQTT client
- Fixed magic number `8` used as array size for `channels_to_publish` in `handle_channel_status` — replaced with `MAX_CHANNELS` so the array size stays in sync if the constant is ever changed
- Fixed `volatile bool` used for `s_mqtt_connected` and `s_mqtt_started` in `mqtt_poolclient.c` — replaced with `atomic_bool` (`<stdatomic.h>`) which provides correct memory-ordering guarantees on all architectures; `volatile` provides no such guarantees and would be unsafe on multi-core targets
- Fixed `led_flash_rx`/`led_flash_tx` blocking the tcp_bridge task for 50 ms via `vTaskDelay` — moved all flash work (set colour → delay → restore) into a dedicated low-priority `led_flash_task`; callers now post a `led_flash_type_t` to a depth-4 queue and return immediately; if the queue is full under burst conditions the flash is silently dropped rather than blocking bus message processing
- Fixed `tcp_bridge_stop` deleting `s_log_mutex` while the task could still be inside `tcp_bridge_vprintf` holding it — replaced `vTaskDelete(handle)` with a cooperative stop: `s_stop_requested` flag causes the task to exit the loop cleanly, close sockets, and give a binary semaphore before calling `vTaskDelete(NULL)`; `tcp_bridge_stop` waits on the semaphore (3s timeout with forced delete fallback) before restoring vprintf and deleting the mutex
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