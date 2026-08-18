# MrDIY ESP32 deferred session control — v0.1.0-dev.26

Date: 2026-08-17–2026-08-18
Status: implementation, build, saved-bond iPhone session, sustained health, hard-reset recovery,
and automatic reconnect passed on the USB bench; signed-release and in-vehicle CAN gates remain

## Physical dev25 finding

The iPhone completed the CRC-valid handshake and firmware notified all handshake chunks. UART then
showed `BLE_APPLICATION_SESSION_READY`, the first post-ready notification, and a deterministic
stack overflow in `vhos_ble_tx`. Dev25 called `vhos_transport_send_health()` and
`vhos_transport_send_session_status()` directly from the 4 KB TX task. Health construction uses a
large JSON payload and framed output buffer, so that control work did not belong on the delivery
task stack.

## Dev26 correction

The BLE TX task now performs only:

1. epoch-bound, encrypted notification delivery;
2. the atomic pending-to-ready transition after every handshake chunk returns `rc=0`;
3. setting `initial_session_publish_pending` and notifying the health/control task.

The existing 6 KB health task wakes immediately, revalidates connection, encryption, stream CCCD,
application readiness, and epoch, then queues initial health and last OTA status through the normal
`SESSION_REQUIRED` path. It logs:

```text
BLE_SESSION_CONTROL_PUBLISH ... health_rc=... status_rc=... stack_high_water=...
```

Queue pressure rearms the pending publish for a bounded periodic retry. Disconnect, GAP connect,
and NimBLE host reset clear the pending flag. No task-stack increase is used as the primary fix.

## Unchanged safety and authorization

- Only the typed handshake response can leave before application readiness.
- Readiness still requires successful notification of all handshake chunks in the same connection
  epoch with encryption and stream CCCD active.
- Initial/periodic health, live CAN, capture export, and OTA status remain session-required.
- Host reset and disconnect clear incremental RX, queued TX, handshake pending, readiness, and the
  deferred control flag.
- TWAI remains listen-only; arbitrary CAN/diagnostic transmit remains absent.
- SoftAP and station Wi-Fi remain disabled on normal boot.
- BLE identity, bonds, restored CCCDs, capture evidence, NVS, and OTA partitions are not erased.

## Exact unsigned artifact

| Field | Value |
| --- | --- |
| Application version | `0.1.0-dev.26` |
| Candidate | `targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin` |
| Size | 1,048,496 bytes |
| SHA-256 | `aa594fc364c5e4b57e11504271b1a6fe6f7adda9dcff9f2b793fb88be3ed5fbc` |
| Smallest application partition | 1,572,864 bytes (`0x180000`) |
| Build free space | 524,368 bytes (`0x80050`, 33%) |
| Build environment | `espressif/idf:v5.5.3`, target `esp32` |
| Signing | unsigned; external release key not selected |

Historical `-signed.bin` and `-unsigned.bin` files embed dev15 and are not dev26 candidates.

## Physical acceptance evidence

Evidence directory: `/tmp/vhos-dev24-acceptance.IbXL3W`

The directory name predates dev26; the individual log names and decoded handshake identify the
firmware actually exercised.

### Saved-bond connection and sustained health

The existing iPhone peripheral identity connected as `link_session=1` at
`2026-08-18T00:18:17.536Z`. Service and characteristic discovery completed, the stream CCCD became
ready at `00:18:17.822Z`, and the first-attempt handshake was verified at `00:18:17.992Z` with:

```text
HANDSHAKE_VERIFIED firmware=0.1.0-dev.26
```

Initial health decoded at `00:18:18.188Z`. The iPhone then decoded 63 recurring health reports
through `00:20:27.787Z`, a 129.599-second continuous window. There was no TX stack overflow,
firmware panic, spontaneous reboot, Pair sheet, manual **Forget This Device**, or NVS erase.

Firmware-side evidence also recorded successful deferred publication:

```text
BLE_SESSION_CONTROL_PUBLISH ... health_rc=ESP_OK status_rc=ESP_ERR_NOT_FOUND \
notification_count=1 stack_high_water=1700
```

`ESP_ERR_NOT_FOUND` is the expected “no prior OTA status” result, not a publication failure. The
positive high-water value and sustained health stream close the dev25 `vhos_ble_tx` overflow.

### Deliberate hard reset and automatic reconnect

A Mac `esptool chip-id` read deliberately hard-reset the attached gateway and confirmed:

- chip: `ESP32-D0WDQ6`;
- hardware MAC: `94:54:c5:b0:8d:14`.

Core Bluetooth reported the interrupted link as `CBError.connectionTimeout` at
`00:20:34.188Z`. The app retained reconnect intent and automatically established `link_session=2`
at `00:21:25.797Z`. Service/characteristic discovery completed again, the current stream CCCD was
ready at `00:21:26.026Z`, and the first-attempt handshake verified dev26 at `00:21:26.215Z`.
Initial health arrived at `00:21:26.365Z` and recurring health continued for the remainder of the
record. This recovery required no Pair sheet, manual Forget, or NVS/bond erasure.

The firmware trace corroborates restored security state and stream subscription, skipped redundant
security initiation on the encrypted bond, successful session readiness, and deferred health
publication.

### Bench interpretation

All decoded health reports contain zero CAN frames, zero dropped frames, zero bus errors, and zero
bus-off events. That is expected: the gateway was powered over USB on the Mac bench and was not
connected to the vehicle DLC/CAN bus. The alternating passive 500/250-kbit probes demonstrate that
the listen-only scanner remained alive; this run does not accept or reject in-vehicle CAN capture.

## Acceptance matrix

| Gate | Result | Evidence |
| --- | --- | --- |
| Existing bond reused without user repair | PASS | link sessions 1 and 2; no Pair/Forget/NVS erase |
| Dev26 handshake and current CCCD | PASS | `00:18:17.992Z` and `00:21:26.215Z` |
| Deferred initial health publication | PASS | `health_rc=ESP_OK`, high-water `1700` |
| Sustained operation without TX overflow/reboot | PASS | 63 health reports over 129.599 seconds before deliberate reset |
| Automatic recovery from deliberate hard reset | PASS | timeout observed, link session 2 connected and verified |
| Listen-only USB-bench behavior | PASS | recurring probe state, zero traffic/errors/bus-off |
| Signed release artifact | PENDING | external release-signing key was not selected |
| In-vehicle CAN traffic and passive lock | PENDING | USB bench was not attached to DLC/CAN |
| Forced mid-handshake CCCD/notify failure | PENDING | separate negative-path field injection |

Dev26 is physically accepted for saved-bond BLE commissioning, delivery-safe session startup,
sustained health, and reset/reconnect recovery. It remains an unsigned development candidate until
release signing is completed, and vehicle-network acceptance remains a separate field run.
