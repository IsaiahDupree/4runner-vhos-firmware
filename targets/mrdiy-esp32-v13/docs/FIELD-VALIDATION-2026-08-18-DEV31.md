# MrDIY ESP32 live-capture/export isolation — v0.1.0-dev.31

Date: 2026-08-18

Status: source and ESP-IDF build verified. The matching iOS 0.3.5 (11) containment is installed;
dev31 is not yet flashed or physically accepted.

## Field failure that required this change

Firmware dev29 connected and recorded vehicle traffic correctly, but the iPhone simultaneously
resumed a previous capture. Health advanced from 3,388 received frames / 21 TWAI misses to 7,807 /
57 and then 8,893 / 66 while 24-record history chunks were transferred. The BLE link dropped, later
physical links could not enumerate GATT, and a later successful session reported reset CAN counters
and new capture identifiers. A second history transfer was followed by another disconnect.

The evidence proves that concurrent live recording and bulk SPIFFS export is unsafe on this target.
It does not identify one exact reset instruction; that still requires retained reset-reason/UART
evidence. The product-side chronology is versioned in the 2026-08-18 BLE capture-sync incident.

## Dev31 correction

Dev31 makes recorder state an enforced firmware boundary rather than a client convention:

1. `capture.read` is rejected while logging is enabled, while a CAN callback is producing a
   retained record, while any record is queued or in flight, or while the writer owns a record.
2. Pausing first disables new observations, drains the writer and queue with a two-second bound,
   acquires the file lock, and flushes the current segment before export can begin.
3. Resuming is serialized by the same file lock and resets sampling buckets so the first new state
   is retained.
4. The read function repeats the quiescence check under the file lock. A request admitted before a
   state change therefore cannot race into an active recorder.
5. Manual rotation is also denied until the recorder is quiescent.
6. Periodic health uses an in-memory capture snapshot. It no longer flushes, stats, reopens the
   previous header, or queries SPIFFS capacity every health interval.
7. Inventory refresh remains an explicit, bounded filesystem operation. While logging is active it
   does not force a current-file flush.
8. Export chunks are reduced from 24 to 12 records, decreasing BLE notification fragmentation.
9. A chunk's session identifier comes from the validated file header read under the same lock; the
   worker no longer performs a second full capture-status scan after each chunk.
10. The handshake adds the ESP-IDF reset-reason value so the iPhone trace can distinguish power,
    watchdog, panic, and software resets without an attached UART.

iOS 0.3.5 complements the server boundary by requesting inventory only when `logging=true`. It
cancels any history task and records
`CAPTURE_SYNC_DEFERRED reason=recorder-active policy=inventory-only`. Evidence remains on the
gateway; it is not silently discarded.

## Safety invariants

- TWAI remains `TWAI_MODE_LISTEN_ONLY`.
- No arbitrary CAN, K-line, J1850, diagnostic, or active-test executor is enabled.
- The capture format, CRC32C, current/previous retention, GATT UUIDs, identity, bonds, NVS, and OTA
  partitions are unchanged.
- A client cannot override the active-recorder export denial.
- Export remains session-required and generation-bound; stale worker output cannot cross a BLE
  disconnect.
- SoftAP and station Wi-Fi remain off during normal operation.

## Exact unsigned artifact

| Field | Value |
| --- | --- |
| Application version | `0.1.0-dev.31` |
| Embedded build ID | `v0.1.0-dev.11-10-gdb1d68d4d5cb` |
| Candidate | `targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin` |
| Size | 1,048,496 bytes |
| SHA-256 | `a4234a4cea591a6759f49457dac39fd1e50237556d46674db348d07dfef5d72e` |
| Smallest application partition | 1,572,864 bytes (`0x180000`) |
| Build free space | 524,368 bytes (`0x80050`, 33%) |
| Build environment | `espressif/idf:v5.5.3`, target `esp32` |
| Signing | unsigned; external release key not selected |

Historical `*-signed.bin` files are not dev31 and must not be selected. The development binary may
be flashed only through the established NVS-preserving application path, or after creating and
verifying a new signed release package.

## Acceptance matrix

| Gate | Result | Evidence required |
| --- | --- | --- |
| ESP-IDF build and partition fit | PASS | exact binary identity above; 33% free |
| Active recorder rejects `capture.read` | BUILD VERIFIED | embedded rejection markers; physical command test pending |
| Health path avoids filesystem refresh | SOURCE VERIFIED | periodic health calls the runtime-only snapshot |
| iPhone inventory-only active-recording policy | INSTALLED | iOS 0.3.5 (11), 45 Swift tests |
| Saved-bond reconnect with active vehicle capture | PENDING | dev31 handshake, recurring health, no chunk requests, no Pair/Forget |
| Zero new TWAI misses during sustained capture | PENDING | compare starting and ending hardware counters under defined traffic |
| Paused full history download | PENDING | stop/drain/flush, complete previous+current export, hashes/record counts |
| Mid-download BLE loss and resume | PENDING | forced disconnect, new contract, durable offset resume |
| Vehicle-power loss and recovery | PENDING | retained segment, reset reason, reconnect, no manual bond repair |
| Signed OTA and rollback | PENDING | external signing key and inactive-slot fault matrix |

Dev31 is not physically accepted until those field gates run against the exact hash above.
