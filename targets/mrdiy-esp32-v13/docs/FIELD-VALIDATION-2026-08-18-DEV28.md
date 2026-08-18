# MrDIY ESP32 stale-bond and capture-export recovery — v0.1.0-dev.27/dev.28

Date: 2026-08-18
Status: dev27 stale-bond recovery and one in-vehicle application session physically passed;
dev27 bulk capture interruption reproduced a firmware-side GATT stall; dev28 correction is built
and requires physical flash/acceptance

## Field sequence

The gateway entered the field with the iPhone still retaining its prior bond while the ESP32
NimBLE store reported zero local and peer security records. The radio advertised and Core
Bluetooth could open a physical link, but the encrypted stream CCCD failed with
`CBATTError.insufficientAuthentication`. Reusing the same random-static identity could not repair
the asymmetric key state because only iOS still possessed the old keys.

Dev27 adds a persistent bond-policy epoch beside the existing random-static identity. When a
gateway that was previously paired boots with an empty NimBLE key store, it rotates its BLE
identity once, preserves the canonical `VHOS-4R-OBD-B08D14` display name, and permits iOS to
discover and bond the replacement identity without **Forget This Device**. A successful pairing
persists `paired_once=1`; healthy reboots with bond records preserve both identity and keys.

The physical dev27 recovery produced:

- new Core Bluetooth UUID `52B37B97-0F8D-D862-9303-57202776396C`;
- successful Secure Connections encryption and `bonded=1`, key size 16;
- first-attempt `HANDSHAKE_VERIFIED firmware=0.1.0-dev.27`;
- recurring CRC-valid health and capture-index frames;
- no manual Forget, NVS erase, or product-name change.

After an unplug/replug into the vehicle, the UI first showed a physical link in **Verifying** and
one second later showed subscribed streams, 38 CRC-valid frames, VHOS firmware mode, and contract
`1.0.0`. This proves that the normal Verifying-to-Connected transition and the dev27 identity/bond
recovery survived that exercised power cycle.

## Dev27 bulk-export failure

The same car-side run exposed a separate failure while resuming a 2,208-record previous capture.
The attached iPhone trace is `/tmp/vhos-dev27-car-drop/iphone-autoscan.log`.

| UTC time | Evidence | Interpretation |
| --- | --- | --- |
| `01:10:04.252` | `LINK_CONNECTED link_session=1` | Saved replacement UUID opened a physical link |
| `01:10:04.548` | `SUBSCRIBE_READY` | Current-link encrypted stream CCCD passed |
| `01:10:04.782` | `HANDSHAKE_VERIFIED firmware=0.1.0-dev.27` | Application contract passed |
| `01:10:04.847` onward | recurring `HEALTH_DECODED` | Live session was functional |
| `01:10:05.193` through `01:10:09.098` | capture chunks advanced from record 432 through 840 | Automatic previous-log download was the active bulk workload |
| `01:10:10.928` | `LINK_DISCONNECTED ... CBErrorDomain code=0` | Link dropped during the resumed bulk transfer |
| `01:10:16.254` onward | physical reconnects succeeded but service discovery timed out | The radio/controller accepted links while GATT no longer answered |
| later filtered scans | gateway repeatedly advertised at approximately -63 to -75 dBm | Gateway power and advertising survived; this was not disappearance of the device |

An independent macOS Core Bluetooth discovery-only probe then found `VHOS` at -54 dBm, connected,
and also timed out before service enumeration. The probe did not subscribe, pair, or modify a
bond. This discriminates a firmware-side GATT-host stall from an iPhone restoration/cache problem
or weak RSSI.

Code inspection identified blocking SPIFFS work in the NimBLE GATT write callback: each
`capture.read` command synchronously opened, sought, and read the capture file before the callback
could return. Even when ordinary reads are fast, filesystem latency and an interrupted bulk
transfer must not own the BLE host event thread.

## Dev28 correction

Dev28 moves capture-file reads to a dedicated 6 KiB, lower-priority export worker:

1. the GATT callback validates the CRC-protected, allowlisted request;
2. it enqueues exactly one `{slot, offset, transport_generation}` request without file I/O;
3. the worker reads SPIFFS and builds the existing versioned capture-chunk response;
4. disconnect/reset increments the transport generation and clears queued requests;
5. an old worker result is discarded unless its generation still matches immediately before
   response admission;
6. the normal BLE connection-epoch/session gate still authorizes every emitted frame.

The queue depth is one because the contract is request/response and the phone must not pipeline
bulk reads. Queue pressure returns an explicit error instead of accumulating unbounded work.
Worker logs report request generation, slot, offset, result, elapsed milliseconds, and task-stack
high-water evidence.

The paired iPhone build `0.3.3 (9)` complements this boundary: it waits three seconds after a
healthy contract, requests one 24-record chunk every 500 ms, and gives a chunk eight seconds to
answer. A timeout or capture-command write error pauses only Recent Logs synchronization; live
health and the BLE application session remain authoritative and connected. Reconnect resumes from
the durable local record offset.

## Safety invariants

- TWAI remains `TWAI_MODE_LISTEN_ONLY`; no arbitrary CAN/diagnostic transmit path was added.
- Capture reads remain read-only and use the existing CRC-protected binary record contract.
- SoftAP and station Wi-Fi remain disabled during normal operation.
- The canonical name, gateway ID, GATT UUIDs, GATT schema epoch, bond keys, captures, NVS, and OTA
  partitions are unchanged by dev28.
- A stale asynchronous result cannot cross a disconnect/reconnect transport generation.
- Background evidence synchronization cannot promote OBD-II confirmation or vehicle health.

## Exact unsigned dev28 artifact

| Field | Value |
| --- | --- |
| Application version | `0.1.0-dev.28` |
| Candidate | `targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin` |
| Size | 1,048,496 bytes |
| SHA-256 | `2ea8ca8e339c50d787d3522ae82e94fa7155a739da7564bb6b424fde0b88abbc` |
| Smallest application partition | 1,572,864 bytes (`0x180000`) |
| Build free space | 524,368 bytes (`0x80050`, 33%) |
| Build environment | `espressif/idf:v5.5.3`, target `esp32` |
| Signing | unsigned; external release key not selected |

Historical signed files are not dev28 candidates. Select the exact application binary above only
after the normal external signing step, or use the NVS-preserving development application flash at
offset `0x10000` for the next attached-device acceptance run.

## Acceptance matrix

| Gate | Result | Evidence |
| --- | --- | --- |
| Asymmetric stale-bond recovery without Forget | PASS on dev27 | one-time identity rotation, fresh encrypted bond, first-attempt handshake |
| Car power-cycle application reconnect | PASS on dev27 | Verifying followed one second later by subscribed streams and contract `1.0.0` |
| Bulk-transfer disconnect reproduced | PASS on dev27 | exact iPhone chronology through record offset 840 and disconnect |
| Firmware vs iPhone failure boundary | PASS | independent strong-RSSI Mac link also timed out on GATT enumeration |
| Dev28 ESP-IDF build and partition fit | PASS | 1,048,496-byte artifact; 33% free |
| NimBLE host remains responsive during capture reads | PENDING | requires dev28 physical flash and interrupted-read injection |
| Full 2,208-record resumable sync | PENDING | requires dev28 plus iOS `0.3.3 (9)` physical run |
| Sustained vehicle CAN capture | PENDING | latest attached trace reported zero frames; ignition/bus conditions were not established |
| Signed release artifact | PENDING | external release-signing key was not selected |

Dev28 is a build-verified development candidate. It must not be described as physically accepted
until the same gateway completes service discovery, handshake, health, full resumable capture
download, an interruption/reconnect, and a second GATT enumeration without a power cycle.
