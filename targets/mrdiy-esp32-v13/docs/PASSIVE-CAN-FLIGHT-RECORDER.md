# Passive CAN flight recorder and low-trip development loop

Status: implemented in `v0.1.0-dev.11`; physical vehicle acceptance pending

## Purpose

The recorder exists so a short visit to the 4Runner produces evidence that can be replayed,
inspected, exported, and analyzed repeatedly away from the vehicle. The ESP32 must not depend on
an iPhone connection at the moment an event occurs. The iPhone must not require the owner to keep
the app foregrounded for the entire capture. Wi-Fi and SoftAP are not part of this data path.

The intended loop is:

1. connect the gateway to the vehicle;
2. let the gateway identify a passive CAN timing candidate;
3. perform a small, timestamped set of vehicle actions;
4. disconnect and leave the vehicle;
5. reconnect the iPhone to the gateway anywhere it has power;
6. allow automatic, resumable BLE synchronization;
7. export NDJSON and run signal-discovery analysis repeatedly on the same evidence.

This minimizes vehicle trips without weakening the rule that every conclusion resolves to its
source observation.

## Safety boundary

The flight recorder is passive and local:

- TWAI remains `TWAI_MODE_LISTEN_ONLY`.
- The transmit queue length remains zero.
- The capture observer receives only frames already accepted by the receive task.
- Capture commands can request an index, read records, rotate local files, pause local logging, or
  resume local logging.
- Capture commands cannot transmit a CAN frame, send an OBD request, change a bitrate directly,
  or bypass signed experiment and parked-vehicle gates.
- BLE characteristics require link encryption.
- Normal operation starts neither Wi-Fi nor the HTTP status surface.

AI authority remains interpretation and proposal only. A log is evidence, not permission to
interact with the vehicle.

## Evidence observed before this milestone

The `dev.10` vehicle run shown by the iPhone observed more than 18,000 gateway-reported frames in
one view and more than 16,000 in a later view, with zero dropped frames and zero bus-off events.
Fifteen bus errors were also reported. Those errors are retained as gateway/controller evidence;
they are not promoted to a vehicle fault. Passive probing may temporarily select a timing that
does not match the live bus, and the firmware must distinguish probe evidence from a confirmed
diagnostic result.

The app therefore keeps these claims separate:

- vehicle-network traffic observed;
- passive CAN timing/identifier-width candidate;
- OBD diagnostic protocol confirmed.

Only an authorized, allowlisted request with a valid response can make the third claim.

## Storage architecture

The classic ESP32 target has a dedicated `storage` SPIFFS partition at offset `0x310000` with a
size of `0x0f0000` bytes. The recorder uses that existing partition; it does not change the two
1.5 MB OTA application slots.

Files:

- `/vhos/current.vhcan` — active segment;
- `/vhos/previous.vhcan` — immediately preceding segment.

Each segment is limited to 430 KiB. At boot or when the active segment reaches the limit, current
becomes previous, the older previous segment is removed, and a new current segment is opened.
This deliberately forms a bounded rolling recorder. It cannot consume the application slots or
grow without limit.

SPIFFS is formatted only if its dedicated partition cannot be mounted. A storage failure is
reported and does not prevent BLE or listen-only CAN observation from starting.

## Sampling policy and why it is explicit

A busy vehicle network can produce thousands of frames per second. Persisting every frame to the
available internal flash would both exhaust the bounded storage quickly and create unnecessary
flash wear. `dev.11` uses identifier-aware sampling:

- a newly observed identifier is retained immediately;
- a changing payload is retained no more often than every 200 ms per identifier, bitrate, and
  identifier-width tuple;
- an unchanged payload is retained once per second;
- remote-request frames are not retained;
- the complete gateway receive counter continues to count every accepted frame.

The policy protects identifier coverage and changing values while providing a useful rolling
window. It is not lossless bus logging and it never pretends to be. Health and index evidence
expose:

- total observed frames;
- sampled frames;
- sample-suppressed frames;
- retained records;
- queue-dropped records;
- storage write failures;
- current and previous segment record counts;
- free storage bytes.

Future hardware that requires lossless, long-duration capture should use the planned microSD
path. It must keep these counters and evidence contracts.

## Binary file contract

### Segment header — 32 bytes

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII `VHCL` |
| 4 | 1 | format version, currently `1` |
| 5 | 1 | record size, currently `36` |
| 6 | 2 | reserved, zero |
| 8 | 4 | random per-segment session identifier, little-endian |
| 12 | 8 | ESP32 monotonic creation time in microseconds |
| 20 | 4 | changed-payload sampling interval in microseconds |
| 24 | 4 | unchanged-payload sampling interval in microseconds |
| 28 | 4 | CRC32C of bytes 0–27 |

Session identifiers mix hardware randomness with monotonic state. They are evidence correlation
keys, not security tokens and not wall-clock timestamps.

### Stored CAN record — 36 bytes

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | record version, currently `1` |
| 1 | 1 | flags: bit 0 extended, bit 1 remote request, bit 2 listen-only |
| 2 | 1 | data length, bounded to 0–8 |
| 3 | 1 | bitrate code: `1` = 500 kbit/s, `2` = 250 kbit/s |
| 4 | 4 | arbitration identifier |
| 8 | 8 | source receive sequence |
| 16 | 8 | ESP32 monotonic observation time in microseconds |
| 24 | 8 | CAN data bytes; bytes beyond DLC are zero |
| 32 | 4 | CRC32C of bytes 0–31 |

The iPhone validates the outer VHOS frame CRC and each stored-record CRC before persistence.

## BLE transfer contract

The existing VHOS envelope remains authoritative. It supplies message type, outer sequence,
monotonic time, payload length, payload CRC32C, and header CRC32C.

### Type 11 — capture-log request

The payload is eight bytes: version, operation, slot, reserved, and a 32-bit record offset.

Operations:

- `0`: return index;
- `1`: read a chunk;
- `2`: rotate the local recorder;
- `3`: pause local logging;
- `4`: resume local logging.

Slots are `0` for current and `1` for previous. The iPhone automatically uses only index and
read. Rotate/pause/resume require an explicit future owner-facing control because they mutate
local evidence retention.

### Type 12 — capture-log index

The JSON index reports session IDs, bytes, records, total/free storage, logging/mount state, and
every observation/sampling/drop/failure counter. The iPhone requests it immediately after a
handshake advertising `evidence.persistent-log`.

### Type 13 — capture-log chunk

The 16-byte chunk header contains version, slot, end flag, starting record offset, record count,
record size, and session ID. Up to 24 stored records follow. The phone requests the next offset
only after the current chunk has decoded and been written locally.

The offset makes transfer resumable across BLE interruption. Previous is synchronized before
current so a new rolling rotation is less likely to overwrite the older unsynchronized segment.

### Type 2 — live CAN observation

The gateway also emits a bounded live preview at up to 2 observations per second. It is useful
for immediate UI feedback. It is not the durable source: the flash-backed records and their
explicit sampling counters are authoritative for later analysis.

## iPhone persistence and export

The iOS app writes validated records as newline-delimited JSON under Application Support, grouped
by gateway and segment session. It deduplicates by gateway ID, session ID, and source sequence.
On every supported handshake it compares local record count with the gateway index and resumes at
the first missing record.

The Evidence tab reports recorder state, observed/retained counts, on-device record count, free
space, queue/write failures, synchronization progress, recent sessions, and the latest live
observation. It exports `passive-can-recent-logs.ndjson` for analysis or AI handoff.

The exported observation contract includes gateway/session identity, source sequence, monotonic
time, bitrate, arbitration ID, frame flags, DLC, data bytes, evidence source, and iPhone ingest
time. It deliberately does not invent a vehicle ID, decoded signal, PID, or wall-clock observation
time. Those facts must be resolved by later evidence-backed processing.

## One-trip field procedure

Before leaving the desk:

1. install the matching `dev.11` app and firmware;
2. verify the app advertises Passive capture, Persistent CAN log, Evidence export, A/B OTA, and
   Rollback self-test;
3. verify Wi-Fi SoftAP remains disabled;
4. open Evidence and confirm `RECORDING`, nonzero free storage, zero queue drops, and zero write
   failures.

At the vehicle:

1. connect the gateway with ignition in the intended test state;
2. wait for vehicle-network traffic and a passive candidate;
3. note the app time and perform one controlled action at a time, leaving several seconds between
   actions—for example lights, brake pedal, HVAC changes, doors, and transfer-case state only when
   operating conditions are safe and consistent with the vehicle manual;
4. do not run an active diagnostic experiment unless the app separately proves PARKED, signed
   plan, allowlist, idle-capture, and owner-approval gates;
5. leave the gateway powered long enough for buffered records to flush;
6. reconnect the app and wait for the sync message to report completion.

Back at the desk:

1. power the gateway from a stable bench USB source if needed;
2. reconnect over BLE; no vehicle or Wi-Fi is required for log transfer;
3. export NDJSON;
4. preserve the original export checksum;
5. replay and analyze the same file until the next experiment has a specific evidence gap.

## Acceptance gates

Firmware acceptance requires:

- ESP-IDF clean build for classic ESP32;
- boot log reports `CAPTURE_STORE_READY` and a nonzero session ID;
- SoftAP remains disabled by default;
- CAN controller reports listen-only;
- captured records have valid inner CRC32C;
- index totals agree with file sizes and 36-byte record boundaries;
- full segment rotation preserves the immediately preceding segment;
- power interruption loses at most the configured write buffer and does not corrupt earlier valid
  records;
- queue drops and storage failures remain zero in the target vehicle workload.

iOS acceptance requires:

- physical-device build succeeds;
- automatic index request follows the dev.11 handshake;
- previous and current segments download in that order;
- disconnect/reconnect resumes rather than restarts a partial segment;
- duplicate chunks do not duplicate NDJSON observations;
- bad outer or record CRC is rejected and surfaced;
- recent logs survive app restart;
- export opens as valid one-JSON-object-per-line evidence;
- existing signed experiment and OTA safety gates remain unchanged.

## Known limits and next milestones

- Internal flash is a bounded sampled recorder, not lossless long-duration logging.
- ESP32 monotonic time must later be mapped to iPhone wall time with an explicit clock-correlation
  observation if cross-device timing accuracy is required.
- Capture markers are defined in the VHOS message registry but are not yet persisted into the same
  segment. Adding owner-labeled markers is the next highest-leverage improvement for signal
  discovery.
- A desktop replay tool should ingest exported NDJSON, rank identifiers and bit positions by
  controlled-action correlation, and emit proposals with evidence references—not silently promote
  guesses to a Vehicle Signal Pack.
- microSD remains the correct future path for lossless or hours-long capture.
