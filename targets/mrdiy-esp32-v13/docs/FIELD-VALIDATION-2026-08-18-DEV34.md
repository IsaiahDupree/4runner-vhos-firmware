# Dev34 BLE transfer-load hardening

## Status

`v0.1.0-dev.34` is build-verified and passed the 2026-08-20 baseline USB installation, preserved
bond, iPhone reconnect, contract handshake, and short health-stream soak described below. This is
not yet full in-vehicle or transfer-load acceptance: nonempty history transfer, live vehicle CAN,
Android, repeated reconnect, RF-fault, and long-soak gates remain open.

## Failure model

The encrypted VHOS stream carries handshake, health, live CAN, and retained-history frames. A
large history response can span many ATT notifications, especially before a larger MTU is active.
Periodic health and history delivery previously competed for the same finite NimBLE/controller
resources. Field symptoms included an application session that could connect and pass the
handshake, then disappear while retained history was being loaded.

The available evidence establishes notification-path load as a risk, but it does not prove one
exclusive root cause for every observed vehicle disconnect. Dev34 therefore adds bounded recovery
and telemetry rather than hiding failures or converting them into vehicle-health conclusions.

## Changes

1. Pace notification chunks at 50 ms.
2. Retry only `BLE_HS_ENOMEM`, `BLE_HS_EBUSY`, and `BLE_HS_EAGAIN` for at most 80 attempts, with
   a 50 ms delay between attempts.
3. Log the first backpressure event, recovery attempt count, and exhausted result with scope,
   connection handle, epoch, and byte offset.
4. If delivery fails after part of a logical frame was sent, clear application readiness, reset
   the outbound queue, and terminate that exact connection epoch. Reconnection starts from a clean
   VHOS frame boundary.
5. Reduce each retained-history response from 12 CAN records to five.
6. Suppress periodic health while a history transfer owns the stream, then resume health when the
   transfer finishes or the transport session resets.
7. Request a 30–45 ms connection interval, zero peripheral latency, and an 18-second supervision
   timeout; log requested and negotiated values.
8. Increase both configured NimBLE MSYS-2 and controller-to-host ACL block counts to 32.
9. Preserve all existing safety boundaries: TWAI remains listen-only, history export still
   requires a quiescent recorder, and no arbitrary CAN or diagnostic transmitter is added.

## Build evidence

- ESP-IDF image: `espressif/idf:v5.5.3`
- Artifact: `build/vhos_mrdiy_esp32_v13.bin`
- Size: `1,048,496` bytes (`0xfffb0`)
- SHA-256: `37d1cd7391b3672fc6739dcc6251b940566f4100f2999977d45b430a95127ab6`
- Application partition free: approximately 33%
- Build result: pass

The artifact is unsigned. Historical files whose names contain `signed` are not dev34 candidates
and must not be flashed as this build.

## 2026-08-20 baseline physical deployment

The connected serial device was positively identified before writing:

- target: MrDIY OBD/CAN gateway;
- silicon: ESP32-D0WDQ6 revision 1.1;
- silicon MAC: `94:54:c5:b0:8d:14`;
- flash: 4 MB at 3.3 V;
- serial path: `/dev/cu.usbserial-0001`.

Only the dev34 application partition was written at `0x10000`. Esptool erased
`0x00010000`–`0x0010ffff`, wrote `1,048,496` bytes, verified the flash hash, and reset the target.
NVS, OTA metadata, the inactive application slot, and capture storage were not erased.

A new full 4 MB read reached the end of flash but failed esptool's final length validation with
`Read more than expected`; it was not retained as a valid recovery image. Recovery coverage was
preserved by the prior known-good 4 MB image plus fresh partition backups:

| Private recovery artifact | Bytes | SHA-256 |
|---|---:|---|
| `esp32-9454c5b08d14-pre-dev12-20260817.bin` | 4,194,304 | `67584837e6c451914c52e1ed4baf1394b684a9b1975213739d2e35f04456f55b` |
| `esp32-9454c5b08d14-pre-dev34-nvs-20260820.bin` | 16,384 | `793816b89a3b740139685e527c845d85394463d2f2028d35435b517445b66422` |
| `esp32-9454c5b08d14-pre-dev34-otadata-20260820.bin` | 8,192 | `8ba3b110139f45443d4f268d1a3373ef99a1718b71d51664531b83ee2d4b91a3` |

The first boot after flashing proved:

- application version `0.1.0-dev.34` loaded from `0x10000`;
- the prior 267,296-byte capture was preserved during rotation;
- TWAI started in listen-only mode with the 512-entry receive queue;
- the bond store remained populated (`our_records=1`, `peer_records=1`);
- the persisted random-static BLE identity remained active;
- SoftAP remained disabled by the default-safe policy;
- POST reported `VHOS_SELF_TEST_PASS` and `vehicle_bus_read_only=true`;
- no panic or reset loop appeared in the captured boot log.

The boot UART trace was 23,968 bytes with SHA-256
`82cdcd0d3cd2b8cdb9b446de86e31b679d1fe6652a947db7a830b62c2d51f5af`.

### iPhone reconnect and stream evidence

Vehicle Health OS `0.3.17 (24)` was installed as an update on the paired iPhone, preserving its app
container, then launched with automatic gateway scanning. The saved bond required no Settings
"Forget This Device" operation.

The final exported connection trace contained 130 records and covered
`2026-08-20T04:14:26.608Z` through `04:17:54.717Z`:

| Event | UTC time |
|---|---|
| connect requested | `04:14:26.782Z` |
| physical link connected | `04:14:27.220Z` |
| encrypted stream subscription ready | `04:14:28.090Z` |
| dev34 handshake verified | `04:14:28.287Z` |
| first health frame | `04:14:28.639Z` |
| last health frame in snapshot | `04:17:54.717Z` |

The trace contains 104 decoded health frames over 206.078 seconds and zero event names matching
disconnect or failure. Its 58,034-byte snapshot has SHA-256
`e1c9ef497eb6d794b85d6cd4a13498620aa3acf7da765301c0557221b53ccf1e`.
Zero vehicle frames during this check are expected because the gateway was USB-powered on the
bench and not attached to the DLC.

This establishes a physical baseline for installation, persisted identity/bond, BLE security,
service subscription, application handshake, and periodic health delivery. It does not exercise
the bulk-history load path that dev34 specifically hardens.

## Device-free coverage

The checked-in real-CAN replay corpus exercises 5,176 records across eight physical capture
sessions. The cross-platform load gates reconstruct the deployed VHOS live-observation format,
fragment it at hostile boundaries, inject loss/corruption/disconnect faults, and feed the same
decoders used by iOS and Android. A 20-pass clean run decodes 103,520 observations exactly.

Those gates prove framing, resynchronization, ordering, source identity, and mobile ingestion.
They do **not** emulate NimBLE scheduling, controller mbuf exhaustion, FreeRTOS task timing, SPIFFS
latency, TWAI hardware queues, RF interference, or phone OS lifecycle behavior. Firmware hardware-
in-the-loop remains necessary for those layers.

## Remaining physical acceptance

The baseline flash, exact-version check, preserved bond, and normal saved-identity reconnect have
passed. The remaining gates are:

1. one nonempty retained-history download with live CAN attached;
2. 20 consecutive disconnect/reconnect/download cycles without Pair or Forget;
3. a 30-minute connected soak while live CAN, persisted capture, and repeated history download are
   active;
4. forced phone-process death, ESP reset, RF loss, and mid-transfer disconnect cases;
5. Android head-unit installation, pairing, sustained ingestion, and iPhone ownership handoff;
6. evidence review for notification backpressure, frame recovery, reset reason, queue pressure,
   dropped-frame provenance, and automatic recorder resume.

Acceptance requires complete history hashes, no unexplained ESP reset, no stale frame crossing a
connection epoch, and a clear distinction between deliberate retention sampling and real receive
loss.
