# MrDIY ESP32 transport and GATT review closure — v0.1.0-dev.24

Date: 2026-08-17
Status: implementation, independent source audit, Swift typecheck, and reproducible firmware build
passed; signing, flash, and physical iPhone acceptance pending

## Review findings closed

Dev24 closes two high-severity review findings without changing vehicle authority.

### Complete-frame application-session proof

The old BLE write callback treated `ESP_OK` from `vhos_transport_ingest()` as an accepted
application command. That return code was ambiguous: the incremental parser also returned
`ESP_OK` after buffering only a partial header or payload. A first command fragment could therefore
set `application_session_ready=true` before a CRC or application contract had been checked.

Dev24 returns a separate `vhos_transport_ingest_result_t` containing:

- `completed_frames`: the number of complete, CRC-valid, allowed frames processed by that call;
- `session_established`: true only after a valid handshake request has been processed and its
  handshake plus initial health responses have both been queued.

The handshake payload must be valid JSON whose parser consumes the entire payload and whose
required values are:

```json
{"contract":"gateway.handshake.request","contract_version":"1.0.0"}
```

Both the header CRC32C and payload CRC32C must pass before JSON validation. Partial fragments return
success to ATT because they were buffered correctly, but report zero completed frames and cannot
open the session gate. Unsupported or malformed frames remain default-denied.

The acceptance proof line is:

```text
BLE_APPLICATION_SESSION_READY handle=<n> completed_frames=<n> proof=crc-valid-handshake-request-and-responses-queued
```

Periodic health still additionally requires a live connection, link encryption, and the stream
CCCD. A valid application handshake cannot bypass those transport gates.

### Epoch-2 to epoch-6 GATT compatibility

An independent audit compared tag `vhos-mrdiy-v0.1.0-dev.14` at commit
`d7ccfb45c178f1f88f4d7244e26c522cb878737b` with the dev24 working tree. The following were exact:

- all five 128-bit UUID definitions;
- custom characteristic order: command, stream, status, OTA status;
- command write, write-without-response, and encrypted-write properties;
- read, notify, and encrypted access properties on all three outbound characteristics;
- GAP, GATT, then VHOS service registration order;
- no-input/no-output Secure Connections bonding and ENC+ID key distribution;
- `sdkconfig.defaults` and ESP-IDF `v5.5.3`.

The value handles are unchanged: command `16`, stream `18`, status `21`, and OTA status `24`; the
three CCCDs remain `19`, `22`, and `25`. Physical logs independently observed outbound subscription
handles `18`, `21`, and `24`. NimBLE persists CCCDs against peer identity plus characteristic value
handle, so the epoch-2 bond and CCCDs are structurally valid under epoch 6.

Dev24 explicitly allowlists only `2 -> 6` and persists epoch 6 without clearing bonds or rotating
`identity_v1`. Missing or unknown epochs still trigger the existing conservative migration. The
boot proof for a device currently storing epoch 2 is:

```text
BLE_GATT_SCHEMA_MIGRATION stored=2 current=6 compatibility=verified-identical-db action=preserve-identity-preserve-bonds
BLE_IDENTITY_READY type=random-static source=persisted gatt_schema=6 address=<same-address>
```

Application multiplexing over stream handle 18 does not alter GATT metadata. A client must have the
stream CCCD enabled to receive multiplexed frames; the field iPhone already persisted that CCCD.

## Exact dev24 build artifact

| Field | Value |
| --- | --- |
| Application version | `0.1.0-dev.24` |
| Local artifact | `targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin` |
| Artifact state | unsigned ESP-IDF application image; signing and physical flash pending |
| Size | 1,048,496 bytes |
| SHA-256 | `789da24e7938c7e09659582f40c59125ceac9c1b15a915b0616e5e2024f7a909` |
| Smallest application partition | 1,572,864 bytes (`0x180000`) |
| Build free space | 524,368 bytes (`0x80050`) |
| Build environment | `espressif/idf:v5.5.3`, target `esp32` |

The checksum identifies the unsigned local candidate only. Signing produces a different artifact
that must receive its own recorded checksum.

## Safety invariants

- TWAI remains forced to `TWAI_MODE_LISTEN_ONLY`.
- No arbitrary CAN transmit, Active Test, or write-capable OBD-II command is compiled.
- SoftAP, station Wi-Fi, and HTTP remain disabled during normal boot.
- GATT write and notification access remains encrypted; bonding and Secure Connections remain on.
- The compatible migration changes only the schema marker from `2` to `6`; it does not erase NVS,
  BLE identity, security records, CCCDs, captures, or OTA metadata.
- Unknown schema state remains fail-conservative and uses the documented one-time migration.
- A partial, malformed, CRC-invalid, wrong-contract, or unsupported application frame cannot make
  periodic health eligible.

## Reproduction commands

Run from the repository root:

```bash
docker run --rm \
  -v "$PWD:/project" \
  -w /project/targets/mrdiy-esp32-v13 \
  espressif/idf:v5.5.3 \
  bash -lc 'idf.py build'

swiftc -typecheck tools/vhos_ble_probe.swift
shasum -a 256 targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin
stat -f '%z bytes' targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin
strings targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin \
  | rg '0.1.0-dev.24|BLE_APPLICATION_SESSION_READY|verified-identical-db'
```

The Mac probe sends `gateway.handshake.request` and exercises the same CRC-framed stream contract.
Run it only while the iPhone is disconnected because the target accepts one BLE central:

```bash
tools/run_vhos_ble_probe.sh 60 | tee /tmp/vhos-dev24-mac-probe.log
```

## Physical acceptance gates

1. Flash a signed dev24 application without erasing NVS.
2. Confirm the pre-flash random-static identity, peer/local bond counts, and iPhone peripheral UUID
   remain unchanged.
3. If the stored schema is 2, confirm the preserve action and persisted epoch 6; if already 6,
   confirm there is no migration action.
4. On a fresh fragmented handshake, confirm no session-ready proof appears for any partial write.
5. Confirm exactly one session-ready proof after the final fragment and immediate CRC-valid
   handshake plus health arrival on iPhone.
6. Send or replay a bad header CRC, bad payload CRC, wrong contract, wrong contract version, and
   unsupported message type; each must leave a new session unready.
7. Repeat three saved-identity reconnects and one gateway power cycle without a Pair sheet,
   redundant security initiation, or manual **Forget This Device**.
8. Confirm listen-only CAN and default-off Wi-Fi in both handshake and live health evidence.

Until this matrix passes on the identified board and iPhone, dev24 is build-verified rather than a
release-accepted image. It does not claim OBD-II confirmation, active diagnostics, vehicle motion,
supply measurement, OTA power-loss acceptance, or decoded vehicle signals.
