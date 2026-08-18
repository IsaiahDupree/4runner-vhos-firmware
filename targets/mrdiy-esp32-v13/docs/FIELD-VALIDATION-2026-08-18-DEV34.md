# Dev34 BLE transfer-load hardening

## Status

`v0.1.0-dev.34` is a build-verified candidate. This record does not claim a physical iPhone,
Android, or in-vehicle acceptance run. Its purpose is to preserve the exact implementation and
define the next hardware test without confusing offline replay success with radio-path proof.

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

## Device-free coverage

The checked-in real-CAN replay corpus exercises 5,176 records across eight physical capture
sessions. The cross-platform load gates reconstruct the deployed VHOS live-observation format,
fragment it at hostile boundaries, inject loss/corruption/disconnect faults, and feed the same
decoders used by iOS and Android. A 20-pass clean run decodes 103,520 observations exactly.

Those gates prove framing, resynchronization, ordering, source identity, and mobile ingestion.
They do **not** emulate NimBLE scheduling, controller mbuf exhaustion, FreeRTOS task timing, SPIFFS
latency, TWAI hardware queues, RF interference, or phone OS lifecycle behavior. Firmware hardware-
in-the-loop remains necessary for those layers.

## Next physical acceptance

Flash dev34 without erasing NVS, confirm the exact firmware version and preserved bond, then run:

1. one normal saved-identity reconnect and CRC-valid handshake;
2. one nonempty retained-history download with live CAN attached;
3. 20 consecutive disconnect/reconnect/download cycles without Pair or Forget;
4. a 30-minute connected soak while live CAN, persisted capture, and repeated history download are
   active;
5. forced phone-process death, ESP reset, RF loss, and mid-transfer disconnect cases;
6. evidence review for notification backpressure, frame recovery, reset reason, queue pressure,
   dropped-frame provenance, and automatic recorder resume.

Acceptance requires complete history hashes, no unexplained ESP reset, no stale frame crossing a
connection epoch, and a clear distinction between deliberate retention sampling and real receive
loss.
