# Solar output and telemetry sample follow-up

This follows solar PRs #115 and #121. The original interpretation of CMD
`0x2C` byte 12 as live activity was based on two snapshots with different
configuration. A later capture with deliberate physical start/stop observations
contradicts that interpretation:

| Selected mode | Physical pump | CMD 0x2C byte 12 | 0x0062 CMD 0x12 payload |
| --- | --- | --- | --- |
| Auto | Running | `02` | `00 04 00` |
| On | Running | `02` | `00 04 00` |
| Off | Stopped | `02` | `00 00 00` |
| Auto | Running again | `02` | `00 04 00` |

These observations came from a Connect 10 with controller firmware 2.6 and
touchscreen 3.10 on 18 October 2026. Byte 12's meaning remains unresolved.
The controller status bit is evidenced on this installation; testing on other
controller firmware is still useful. The full packet layouts are in PROTOCOL.md.

Mode is decoded from CMD `0x2C` byte 11; activity is decoded independently from
source `0x0062`, CMD `0x12`, payload byte 1 bit 2 (`0x04`). A mode request or
configuration report cannot refresh an actual-output sample. The status API
reports mode, nullable activity, output-sample age, the controller status byte
and the source used. Existing heater-type guards remain in place. Relay Solar
(`0x08`) is unaffected by the dedicated Solar bit.

## Temperature and RPM history

Publishing temperature sensor 1 previously copied both sensor slots into the
MQTT cache. Sensor 2 could then be suppressed before it was ever published.
The publisher now sends each real sensor report independently, including
unchanged values. Decoder publication checks each value in the current frame:
an invalid or missing reading cannot republish an old cached reading as new.
A valid roof reading can publish even if the water reading is invalid.
Alternate CMD `0x31` and the water-temperature register mirror remain log-only.

Fresh pump reports are also published when RPM and watts are unchanged.
Temperature and RPM discovery now use `state_class: measurement` and
`force_update: true`. Home Assistant can record actual repeated samples, so
constant temperatures/RPM do not disappear between changes. This adds recorder
events for those real samples; no periodic cached-value replay has been added.
The existing inferred pump-off transition remains supported. No gaps are
backfilled, and genuine missing reports can still leave gaps.

## Validation and scope

Run `bash test/run_tests.sh`. Tests cover both temperature slots changing
together, unchanged reports, invalid/missing values, disconnected MQTT,
independent Solar mode/output, output before channel discovery, stale channel
tuples/registers, unmapped bits, heater/service coexistence and relay Solar.
The packet replay fixture labels captured payloads separately from synthetic
adversarial combinations. This branch adapts the fixes to current upstream;
its full merged firmware still needs physical verification on hardware.

The changes do not modify bus transmission, device addresses, heater freshness
policy, custom valves, build/release workflows or firmware update repositories.
They do not claim to decode the EQ45 G4 fault or resolve external dashboard
verification labels.
