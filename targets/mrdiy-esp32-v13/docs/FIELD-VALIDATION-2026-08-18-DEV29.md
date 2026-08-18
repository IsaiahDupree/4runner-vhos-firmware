# MrDIY ESP32 dev29 capture retention and communication fault acceptance

Date: 2026-08-18

Status: build verified and physically accepted for saved-bond BLE commissioning, application
process loss, and ESP execution-loss recovery on the USB bench. Vehicle CAN, nonempty interrupted
capture export, true rail loss, and signed-release acceptance remain separate gates.

## Changes closed by this candidate

Dev29 rolls three related corrections into one identifiable field candidate:

1. **Asymmetric stale-bond recovery from dev27.** The gateway records that it has paired before.
   If its NimBLE key database later becomes empty while iOS retains the old key, it rotates the
   persisted random-static identity once and permits a fresh secure bond without requiring the
   owner to open Settings and forget the old peripheral.
2. **Nonblocking capture export from dev28.** A CRC-valid `capture.read` command now admits one
   `{slot, offset, transport_generation}` request to a depth-one queue. A dedicated 6 KiB worker
   performs SPIFFS reads outside the NimBLE GATT callback. Disconnect or host reset increments the
   transport generation and clears pending work so an old result cannot cross link epochs.
3. **Previous-segment retention from dev29.** Boot rotation promotes `current` to `previous` only
   when the current segment contains at least one complete capture record. A header-only or empty
   current segment can no longer delete a nonempty previous vehicle capture merely because the
   gateway rebooted on a USB bench with no CAN traffic.

The iPhone `0.3.3 (9)` client complements the firmware boundary. It permits only one capture read
at a time, delays background synchronization until the live contract is healthy, paces 24-record
chunks, and pauses only evidence synchronization after a bounded chunk timeout. Live health and
the BLE session are not reclassified as failed solely because a stored-log read is delayed.

## Communication fault oracle

Physical acceptance did not use the iOS Settings label as proof. Every fault cycle had to produce,
in order, a new `LINK_CONNECTED`, an exact
`HANDSHAKE_VERIFIED firmware=0.1.0-dev.29`, and five subsequent CRC-decoded health frames inside a
55-second recovery budget. The harness also rejects ESP panic, heap corruption, assertion,
stack-overflow, and task-watchdog signatures and rejects iOS frame-decode or exhausted-handshake
signatures.

The strict run retained baseline plus six injected faults:

| Cycle | Fault | Recovery to five health frames |
| ---: | --- | ---: |
| 0 | baseline | 36.242 s; included inherited-link cleanup and an incidental -96 dBm scan |
| 1 | CP2102 RTS hard reset | 15.642 s |
| 2 | connected iPhone app termination/relaunch | 15.421 s |
| 3 | CP2102 RTS hard reset | 23.528 s |
| 4 | connected iPhone app termination/relaunch | 15.721 s |
| 5 | CP2102 RTS hard reset | 15.674 s |
| 6 | connected iPhone app termination/relaunch | 15.301 s |

The first strict attempt exposed a real iOS restoration defect: an already-connected
`CBPeripheral` inherited from the previous app process carried notification state owned by that
old process. The corrected app retires and cancels every inherited connected/connecting object,
preserves the verified UUID and iOS-managed bond, waits for the disconnect boundary, and then
creates one fresh physical link, delegate, CCCD subscription, and handshake. The repeated matrix
passed without a Pair sheet, **Forget This Device**, NVS erase, or manual Connect.

Stopped product-side evidence:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `iphone.log` | 52,830 | `8e69b27fee14ccfeabaf782358c37c08c841a93ec3651392836fdc87892e445a` |
| `esp32.log` | 112,808 | `53039c2dad6a0a34163fb8df14e3b9c34aa1b873cb6df748ac743e2b3e36b6f9` |
| `summary.json` | 5,001 | `02f66748be69083ff5de496da36e86f9a0fe5dbc9b699ea2e9178365fd4fe43e` |

The raw logs remain local engineering evidence. Their versioned facts and stopped-file hashes are
the public record; no `/tmp` path is presented as a downloadable artifact.

## Exact unsigned artifact

| Field | Value |
| --- | --- |
| Application version | `0.1.0-dev.29` |
| Candidate | `targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin` |
| Size | 1,048,496 bytes |
| SHA-256 | `291e1d5d119f0e8eee70b902a79f6ee6138c296946ac4622e11b52816ebbb6a9` |
| Smallest application partition | 1,572,864 bytes (`0x180000`) |
| Build free space | 524,368 bytes (`0x80050`, 33%) |
| Build environment | `espressif/idf:v5.5.3`, target `esp32` |
| Signing | unsigned; external release key not selected |

Historical `*-signed.bin` files are not dev29 candidates. The handshake version and exact hash
must both match before selecting a development artifact, and a public release still requires the
external signing and release-validation path.

## Safety invariants

- TWAI remains `TWAI_MODE_LISTEN_ONLY`; no arbitrary CAN or diagnostic transmit API was added.
- The background worker reads only the existing CRC-protected capture format.
- Queue depth one enforces request/response flow control instead of hiding pressure in memory.
- Connection/session generation is revalidated immediately before response admission.
- Empty bench boots preserve the latest nonempty previous capture rather than manufacturing an
  empty replacement.
- SoftAP and station Wi-Fi remain off during normal BLE operation.
- A USB-bench pass does not confirm OBD-II, vehicle motion, supply voltage, protocol compatibility,
  or any decoded vehicle signal.

## Acceptance matrix

| Gate | Result | Evidence |
| --- | --- | --- |
| ESP execution-loss recovery | PASS | three hard resets; exact dev29 handshake and five health frames after each |
| iPhone app-process-loss recovery | PASS | three terminations; inherited link retired and fresh current-process contract verified |
| Existing-bond reuse | PASS | one local and one peer bond retained; no owner pairing action |
| Wrong-firmware rejection | PASS by harness design | exact handshake version is a mandatory oracle |
| Crash/corruption signature rejection | PASS by harness design | iOS and ESP fatal-signature scans are release gates |
| Dev29 ESP-IDF build and fit | PASS | exact artifact above; 33% application partition free |
| True electrical power interruption | PENDING | requires an explicit per-port switchable hub/relay or production power fixture |
| Nonempty capture interruption/resume | PENDING | requires retained vehicle data and a forced disconnect during chunk export |
| Sustained vehicle CAN capture | PENDING | no vehicle observation may be inferred from the USB bench |
| Signed release artifact and OTA power-loss rollback | PENDING | external key and inactive-slot fault matrix required |
