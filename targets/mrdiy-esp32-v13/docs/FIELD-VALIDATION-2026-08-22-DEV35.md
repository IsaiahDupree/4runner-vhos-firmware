# Dev35 history-transfer safety heartbeat

## Status

`v0.1.0-dev.35` is a build-verified development candidate. Device-free source-contract validation
and the ESP-IDF compile/link/image gates pass. It has not been flashed or physically accepted in
this change. A nonempty in-vehicle retained-history transfer, repeated reconnect, and sustained
mobile freshness test remain required before dev35 can be called field-accepted.

## Failure model

Dev34 deliberately stopped periodic `gateway.health` frames while retained history owned the
encrypted notification stream. That reduced notification competition, but it also removed the
only current gateway-health authority seen by the mobile clients. The iPhone correctly expires a
health observation after five seconds. A healthy history download could therefore make its
listen-only, capture-session, and future deterministic-Park prerequisites become stale even though
the BLE link itself remained alive.

The returned vehicle evidence also exposed a separate lineage issue in the phone decoder, but that
does not change this firmware responsibility: bulk evidence transfer must not silence the current
in-memory gateway heartbeat.

## Implementation

1. `gateway.health` remains scheduled every two seconds while history transfer is active.
2. Health reads capture counters from `vhos_capture_store_get_runtime_status`, which copies cached
   in-memory state under its short critical section and performs no SPIFFS traversal.
3. Health and history remain on the existing encrypted, connection-epoch-bound, CRC-protected
   stream. The queue stays FIFO; health is not inserted ahead of an already admitted logical frame.
4. A health frame may wait up to 500 ms for queue admission. Other stream traffic keeps the existing
   nonblocking admission policy, so this does not turn capture or live-CAN producers into blocking
   work.
5. A failed health admission is logged as `BLE_PERIODIC_HEALTH_QUEUE_FAILED` with transfer state and
   cadence. It is not converted to a success or hidden as a vehicle condition.
6. Partial-frame delivery retains dev34's bounded NimBLE backpressure recovery and exact
   connection-epoch termination behavior.

## Safety and authority boundary

Dev35 does **not** infer Park from zero speed, stationary CAN traffic, capture state, phone input,
or an experiment label. The gateway continues to report:

```json
{"vehicle_motion":"UNKNOWN"}
```

That means firmware-side motion authority remains fail-closed. The heartbeat only keeps genuine
gateway facts current; it cannot unlock OTA, diagnostics, experiments, or any other operation that
requires deterministic `PARKED`. The release validator rejects a MrDIY source tree that replaces
the literal with `PARKED` or `MOVING` before a target-validated authority source is implemented.

TWAI remains `TWAI_MODE_LISTEN_ONLY`, and no raw CAN or arbitrary diagnostic transmit executor is
added by this change.

## Device-free verification

The following checks are required for this candidate:

| Gate | Expected evidence |
| --- | --- |
| Python syntax | `tools/validate_vhos_release.py` compiles without error. |
| Source heartbeat contract | Named 2,000 ms cadence, 500 ms health-only queue wait, no transfer suppression or early-continue path, explicit failure telemetry. |
| Motion authority contract | `UNKNOWN` present; no hard-coded `PARKED` or `MOVING` gateway-health output. |
| Listen-only contract | `TWAI_MODE_LISTEN_ONLY` present and no `twai_transmit` in target source. |
| ESP-IDF build | Classic ESP32 target configures, compiles, links, and produces the application image. |
| Image identity | Project and handshake both report `0.1.0-dev.35`. |

The isolated local build used ESP-IDF `v5.5.3`, configured the classic ESP32 target from the checked
in target configuration, compiled both changed sources, linked the application, passed partition
size checks, and generated these unsigned development artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `vhos_mrdiy_esp32_v13.bin` | 1,048,496 | `2b5897ef5987d6cb904bf65f6297a5ea1719b28dbc1fd165684cabc9c4660f7f` |
| `vhos-mrdiy-esp32-v13-v0.1.0-dev.35-unsigned-merged.bin` | 1,114,032 | `6aa0a6295072a98a9aba2006a0dbd77054bf9862111b8a9cf576b9e2bbd6a38d` |

The application uses 1,048,496 of 1,572,864 bytes in the smallest application partition, leaving
524,368 bytes (33%). Binary inspection confirms the project version, handshake version, heartbeat
continuation/failure telemetry, and literal `UNKNOWN` motion state; it finds no dev34 suppression
telemetry or hard-coded `PARKED`/`MOVING` health state. Release signing and browser-flasher
packaging remain separate, key-gated steps and must not be implied by these unsigned development
images.

## Remaining physical acceptance

1. Positively identify the MrDIY OBD gateway before any write and retain the current private
   recovery coverage.
2. Install dev35 without erasing NVS, capture storage, OTA metadata, or the inactive slot.
3. Prove the saved iPhone bond reconnects without Pair, Forget, or identity rotation.
4. Pause the recorder, download a nonempty retained vehicle capture, and observe health arrivals
   throughout the transfer. The maximum inter-arrival interval must remain below the iPhone's
   five-second freshness limit with no fabricated motion state.
5. Repeat disconnect/reconnect/download at least 20 times, then run a 30-minute live-CAN soak with
   periodic history transfer.
6. Inspect UART and mobile traces for queue-admission failure, NimBLE backpressure, partial-frame
   epoch termination, resets, stale-session leakage, complete capture hashes, and automatic recorder
   resume.
7. Run the equivalent Android ingestion test. Android now uses a five-second freshness window,
   which admits one delayed two-second heartbeat interval plus scheduling jitter; sustained transfer
   still has to demonstrate that window in a physical load test.
