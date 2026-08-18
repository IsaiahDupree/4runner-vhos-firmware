# MrDIY ESP32 delivery-safe session gate — v0.1.0-dev.25

Date: 2026-08-17
Status: implementation, ESP-IDF build, source invariants, and artifact inspection passed; signing,
flash, and physical iPhone acceptance pending

## Findings closed

Dev24 proved that only a complete CRC-valid handshake could set `application_session_ready`, but
two output-path gaps remained:

1. the TX task required only a connection and restored stream CCCD, so passive live-CAN frames
   could leave the gateway before the application contract was established;
2. queue admission did not prove successful notification, so readiness could become true while a
   queued handshake response was later dropped or failed in the TX task.

Dev25 closes both and preserves the complete-frame validation added in dev24.

## Authorization invariant

Every outbound item has one of two explicit scopes and records the connection handle and epoch that
admitted it:

| Scope | Valid frame type | Required state |
| --- | --- | --- |
| `BOOTSTRAP_HANDSHAKE` | handshake response only | active connection + encryption + stream CCCD |
| `SESSION_REQUIRED` | initial/periodic health, live CAN, capture, OTA, and all other responses | bootstrap state + `application_session_ready` |

The firmware checks this table when a frame enters the queue, when the TX task removes it, and
before every ATT notification chunk. The bootstrap scope is rejected if its VHOS message type is
not handshake. This prevents future callers from relabeling arbitrary output as bootstrap.

Before command bytes enter the incremental parser, the GATT callback also proves that the callback
belongs to the active connection, encryption is recorded, and the stream CCCD is active. A missing
precondition emits:

```text
BLE_COMMAND_REJECT ... reason=bootstrap-transport-not-ready
```

`process_frame` and the GATT callback only record that the CRC-valid request was accepted and its
handshake response is pending:

```text
BLE_APPLICATION_HANDSHAKE_PENDING ... proof=crc-valid-request-and-response-queued
```

The TX task sets `application_session_ready` only after every handshake-response chunk returns
`rc=0`, the handle and connection epoch still match admission, and encryption plus the stream CCCD
remain active:

```text
BLE_APPLICATION_SESSION_READY ... proof=crc-valid-request-and-handshake-response-notified
```

Any notify, allocation, state, or epoch failure clears handshake-pending state and leaves readiness
false. Only after the successful transition does TX queue initial health and any last OTA status on
the normal session-required path.

## Link and host epoch boundaries

`DISCONNECT` already cleared the session flag, notification state, TX queue, and incremental RX
buffer. Dev25 gives NimBLE host reset the same boundary:

```text
BLE_HOST_EPOCH_CLEARED reason=<n> rx=reset tx=reset
```

The reset callback first invalidates the connection epoch and all readiness flags, then resets the
queue and transport parser. A partial command, old bootstrap response, or queued live frame cannot
be completed or delivered by a later host epoch.

## Exact dev25 unsigned build

| Field | Value |
| --- | --- |
| Application version | `0.1.0-dev.25` |
| Selected candidate | `targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin` |
| Artifact state | unsigned ESP-IDF application image; do not distribute or use for production OTA |
| Size | 1,048,496 bytes |
| SHA-256 | `4fa64a7ebc121e8db6464cb576eb967dfb9bb2442376dff28b58ff0dafc334b5` |
| Smallest application partition | 1,572,864 bytes (`0x180000`) |
| Build free space | 524,368 bytes (`0x80050`) |
| Build environment | `espressif/idf:v5.5.3`, target `esp32` |

The build directory also contains historical `-signed.bin` and `-unsigned.bin` files whose embedded
version is `0.1.0-dev.15`. They are stale and are not dev25 candidates. Never choose an artifact by
glob or filename suffix; verify the embedded version and checksum above. The external release
signing key was not selected for this run, so no dev25 signed image was produced.

## Safety invariants

- TWAI remains forced to `TWAI_MODE_LISTEN_ONLY`.
- Passive CAN capture continues autonomously on the gateway, but BLE live-CAN export is
  session-required.
- No arbitrary CAN transmit, Active Test, or write-capable OBD-II command is compiled.
- SoftAP, station Wi-Fi, and HTTP remain disabled on normal boot.
- GATT command and notification paths remain encrypted and bonded.
- Epoch `2 -> 6` remains the only explicitly audited bond-preserving GATT migration.
- Resetting session buffers does not erase BLE identity, bonds, CCCDs, NVS, capture evidence, or
  OTA partitions.

## Reproduction and inspection

Run from the repository root:

```bash
docker run --rm \
  -v "$PWD:/project" \
  -w /project/targets/mrdiy-esp32-v13 \
  espressif/idf:v5.5.3 \
  bash -lc 'idf.py build'

swiftc -typecheck tools/vhos_ble_probe.swift
git diff --check
shasum -a 256 targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin
stat -f '%z bytes' targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin
strings targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin \
  | rg '0.1.0-dev.25|BLE_HOST_EPOCH_CLEARED|HANDSHAKE_DELIVERY_FAILED|proof=crc-valid'
```

## Physical acceptance gates

1. Sign dev25 with the external release key and record the signed checksum separately.
2. Flash the signed application at the application boundary without erasing NVS.
3. On a restored encrypted link with the stream CCCD but before sending a handshake, confirm no
   health, live-CAN, capture, or OTA frame arrives.
4. Send a fragmented canonical handshake. Confirm no bootstrap response before the final fragment,
   then receive the handshake; confirm readiness is logged only after all of its chunks return
   success, followed by session-required initial health.
5. Confirm live CAN and periodic health begin only after `BLE_APPLICATION_SESSION_READY`.
6. Disable the stream CCCD and attempt a command; confirm the command is rejected and readiness
   cannot be created from undeliverable output.
7. Force a development host reset with a partial command and pending TX item; confirm both are
   cleared and cannot cross into the next host epoch.
8. Force a bootstrap notification failure or CCCD removal mid-handshake; confirm
   `BLE_APPLICATION_HANDSHAKE_DELIVERY_FAILED`, no readiness transition, and no session-required
   traffic.
9. Repeat three saved-identity reconnects and one gateway power cycle without a Pair sheet,
   redundant security initiation, or manual **Forget This Device**.
10. Reconfirm listen-only CAN and default-off Wi-Fi in handshake and health evidence.

Until this matrix passes on the identified ESP32 and iPhone, dev25 is an unsigned, build-verified
candidate rather than a release-accepted image.
