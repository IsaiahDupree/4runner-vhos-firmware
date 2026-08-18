# Dev32 CAN acquisition-quality foundation — 2026-08-18

## Scope

This record documents the build and evidence boundary for `v0.1.0-dev.32`. It does not claim
physical vehicle acceptance. The change exists to reduce and, more importantly, attribute CAN
receive loss without confusing deliberate flash sampling with missing vehicle traffic.

## Dev31 vehicle baseline

The final health report from the preceding vehicle run contained:

| Measurement | Result |
| --- | ---: |
| Passive network candidate | `CAN_11_500` |
| Valid TWAI frames | 29,200 |
| Standard / extended frames | 29,200 / 0 |
| Aggregate TWAI missed + overrun | 230 |
| Aggregate receive-loss ratio | approximately 0.78% |
| Bus errors | 15 |
| Bus-off events | 0 |
| Retained records after reboot/flush | 1,984 |
| Capture queue drops / write failures | 0 / 0 |

The recorder intentionally persists changing identifiers more frequently than stable ones. Its
retained-record density is therefore not a receive-loss counter.

## Implementation

The passive bus remains `TWAI_MODE_LISTEN_ONLY`. No CAN transmit path was added.

1. The ESP-IDF TWAI driver receive queue is 512 entries.
2. A priority-12 receive task on core 1 drains up to 64 frames per scheduling batch.
3. Each received frame becomes a complete observation with source sequence, gateway monotonic
   time, arbitration metadata, bitrate, DLC, and payload.
4. A 256-entry queue transfers observations to a priority-9 dispatch task on core 0.
5. Capture retention, passive J1979 recognition, and live BLE evidence execute from dispatch,
   outside the critical receive loop.
6. The BLE TX task uses a 6 KiB stack because its bounded frame buffer increased to 1,600 bytes;
   the health/control task uses 8 KiB while constructing the expanded health contract.

On single-core targets both tasks use no affinity. Queue insertion is nonblocking, so overload is
visible as an explicit observer-drop counter instead of silently stalling TWAI receive.

## Health and evidence contract

The additive `gateway.health` fields are:

- `can_twai_receive_missed_frames`;
- `can_twai_receive_overrun_frames`;
- `can_twai_receive_queue_depth` and `can_twai_receive_queue_capacity`;
- `can_observer_queue_dropped_frames`;
- `can_observer_queue_depth`, `can_observer_queue_high_water`, and
  `can_observer_queue_capacity`.

The existing capture fields retain their distinct meanings: observed, sampled,
policy-suppressed, retained, capture-queue-dropped, and storage-write-failed. The aggregate
`dropped_frames` value is the sum of TWAI missed, TWAI overrun, and observer-queue drops; it does
not include policy-suppressed flash records.

## Safe full-history transfer

A capture-control pause can now identify history transfer separately from OTA. The recorder is
flushed before export. If that BLE transport session resets before the phone sends resume, the
gateway automatically restarts recording. OTA remains a deliberate fail-closed pause and is not
auto-resumed by this rule.

## Build evidence

- ESP-IDF: `v5.5.3`
- target: classic ESP32 / MrDIY CAN Shield v1.3+
- application image: `build/vhos_mrdiy_esp32_v13.bin`
- size: 1,048,496 bytes (`0xfffb0`)
- unsigned SHA-256: `8486858f2330416479a306d4e107f6cf152d83739e4a4a6d6f032be05580f022`
- smallest app partition: `0x180000`
- remaining app-partition space: `0x80050` bytes (33%)
- static DRAM: 54,132 / 124,580 bytes (43.45%)
- IRAM: 121,071 / 131,072 bytes (92.37%)

The container build and size checks passed. The artifact is unsigned because the external release
signing key is intentionally unavailable in the repository. Do not substitute an older signed
artifact with a different embedded version.

## Physical acceptance gates

Before flashing dev32, synchronize the preserved 1,984-record dev31 vehicle capture. Then:

1. flash the application without erasing NVS, bonds, or the capture partition;
2. prove the saved iPhone bond and application handshake still recover;
3. record at least five parked vehicle minutes at the same operating state;
4. require zero observer drops, capture-queue drops, storage-write failures, and bus-off events;
5. compare TWAI missed and overrun independently against the dev31 0.78% aggregate baseline;
6. force a BLE loss during approved history transfer and prove the recorder resumes;
7. replay the exported raw records before assigning any Toyota signal semantics.

No identifier is promoted into the 4Runner Vehicle Signal Pack until synchronized SAE J1979 or
Toyota Techstream evidence validates meaning, byte order, scaling, offset, and operating-state
scope.
