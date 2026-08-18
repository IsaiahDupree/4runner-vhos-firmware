# MrDIY ESP32 passive SAE J1979 evidence — v0.1.0-dev.30

Date: 2026-08-18
Status: implementation and ESP-IDF build passed; physical passive-response and in-vehicle gates are pending

## Outcome

Dev30 adds a loss-bounded, passive path from a real CAN observation to a versioned VHOS
`DIAGNOSTIC_RESPONSE` frame. It does **not** turn the gateway into an unrestricted scan tool and it
does not change `TWAI_MODE_LISTEN_ONLY`.

The immediate field workflow is:

1. connect Techstream or another owner-approved read-only OBD client;
2. let that client issue Mode 01 queries while the VHOS gateway remains passive;
3. recognize positive single-frame replies on `0x7E8`–`0x7EF`;
4. send those raw replies, with acquisition lineage, to the iPhone/Android decoder;
5. enumerate supported PIDs separately for each ECU before interpreting any value.

This lets the project validate acquisition and decoding without silently broadening firmware
authority.

## Accepted passive input

`vhos_j1979_decode_passive_response` accepts an observation only when all of these are true:

- standard 11-bit CAN frame;
- not a remote-request frame;
- response identifier is in `0x7E8`–`0x7EF`;
- observation bitrate is exactly 500 or 250 kbit/s;
- ISO-TP PCI nibble identifies a single frame;
- declared application length is `2...7` and fits the CAN DLC;
- first application byte is the positive Mode 01 response `0x41`.

Negative responses, multi-frame traffic, other diagnostic services, extended identifiers, and
malformed length fields are not promoted into J1979 evidence. Their original CAN observations
remain available in the passive capture.

Supported-PID bitmap decoding is bounded to `0x01...0xFF`; the notional bit beyond PID `0xFF` in
the terminal `0xE0` page is ignored instead of wrapping to zero.

## BLE wire evidence

The worker emits message type `3` with an exact 36-byte payload:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 1 | format version `1` |
| 1 | 1 | transport: `1` = 11-bit/500 kbit/s, `2` = 11-bit/250 kbit/s |
| 2 | 1 | positive response byte count, `2...7` |
| 3 | 1 | reserved zero |
| 4 | 4 | ECU CAN identifier, little endian |
| 8 | 8 | original CAN `source_sequence`, little endian |
| 16 | 8 | gateway monotonic capture time in microseconds, little endian |
| 24 | 4 | capture-session identifier, little endian |
| 28 | 7 | raw positive response beginning with `41 <PID>` |
| 35 | 1 | reserved zero |

The surrounding VHOS frame supplies protocol version, logical sequence, monotonic time, header
CRC32C, and payload CRC32C. The existing BLE session gate requires an encrypted, subscribed,
CRC-valid application handshake before this record can be queued or delivered.

The CAN observer performs no JSON encoding, BLE write, or filesystem access. It attempts one
nonblocking send into a depth-16 queue. Saturation increments `J1979_PASSIVE_QUEUE_FULL`; it never
stalls the lossless capture path.

## Supported-PID enumeration contract

The gateway exposes a pure planner for the fixed functional request:

```text
CAN ID 0x7DF
02 01 <base PID> 00 00 00 00 00
```

Only bases divisible by `0x20` are accepted. The planner returns `ESP_ERR_INVALID_STATE` unless
all four independent predicates are true:

- signed experiment plan verified;
- vehicle deterministically PARKED;
- capture idle;
- CAN/OBD protocol confirmed.

The production target has no caller that can provide that context, no executor that sends the
planned frame, and no arbitrary diagnostic command. TWAI remains listen-only. This is deliberate:
the request bytes and validation logic can be reviewed now without making “unknown motion” a
permission to transmit.

## Exact unsigned build

| Field | Value |
| --- | --- |
| Application version | `0.1.0-dev.30` |
| Candidate | `targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin` |
| Size | 1,048,496 bytes |
| SHA-256 | `ded332645e551e4b66f5e6c9077744cbd1615e7f48ef900a6012ca7c2e66561b` |
| Smallest application partition | 1,572,864 bytes (`0x180000`) |
| Build free space | 524,368 bytes (`0x80050`, 33%) |
| Build environment | `espressif/idf:v5.5.3`, target `esp32` |
| Signing | unsigned; external release key not selected |

The build log and embedded strings identify dev30 and the passive/default-deny diagnostics. Any
historical `*-signed.bin` file is not this candidate and must not be flashed as dev30.

## Verification completed

- Docker ESP-IDF 5.5.3 clean build: PASS.
- Application size and partition fit: PASS.
- Passive decoder and supported-bitmap code compiled into the target: PASS.
- Handshake embeds `0.1.0-dev.30`: PASS.
- Normal-boot SoftAP policy unchanged: PASS by source/build review.
- TWAI listen-only configuration unchanged: PASS by source/build review.
- No production caller/executor for the request planner: PASS by source review.

## Physical gates still open

- observe a real `0x7E8`–`0x7EF` positive response while Techstream issues Mode 01 queries;
- verify the iPhone and Android decoders preserve identical ECU/PID/sequence/time fields;
- complete each responding ECU's `00/20/40/...` continuation chain;
- compare standard decoded values with Techstream at synchronized timestamps;
- demonstrate queue saturation is reported and never blocks capture;
- separately review and approve any future transition out of listen-only mode.

Until those gates close, dev30 proves an implemented and build-valid passive evidence path—not an
active in-vehicle diagnostic session.
