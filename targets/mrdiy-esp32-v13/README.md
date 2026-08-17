# VHOS target: ESP32 + MrDIY CAN Shield v1.3+

This target is for the physically verified classic ESP32-D0WDQ6 board running the
MrDIY CAN Shield v1.3+ pinout. It is not compatible with the WiCAN Pro ESP32-S3
image.

## Hardware contract

- MCU: classic ESP32, 4 MB flash
- CAN RX: GPIO 4
- CAN TX: GPIO 5
- CAN controller mode: forced `TWAI_MODE_LISTEN_ONLY`
- Initial passive bus timing: 500 kbit/s, followed by bounded 500/250-kbit observation windows
- No raw transmit, ELM327 command, or active diagnostic path is compiled into this target

The pin assignment and 500 kbit/s factory default are traceable to the recovered
device image and the upstream MrDIY CANaBus v1.3+ firmware. They do not, by
themselves, prove a vehicle protocol.

## Gateway health foundation

The image exposes the VHOS BLE service used by the iOS app and reports actual
gateway health: received frames, controller drops, bus errors, bus-off transitions,
the enforced listen-only state, and the passive capture-store status. Supply voltage, motion,
protocol confirmation, active OBD queries, and Wi-Fi OTA upload remain unavailable until their real
implementations land; the app therefore shows those states as unavailable or pending.

Firmware `v0.1.0-dev.10` alternates bounded 10-second listen-only windows at 500 and 250 kbit/s
until at least three valid CAN frames establish a passive lock. It reports the active bitrate,
probe state, standard/extended frame counts, per-bitrate counts, and a passive CAN candidate. A
passive lock proves only that valid CAN traffic was observed; `obd_protocol_confirmed` remains
false until a separately authorized diagnostic read succeeds. See
[passive CAN discovery](docs/PASSIVE-CAN-DISCOVERY.md).

Firmware `v0.1.0-dev.11` adds an always-on, bounded passive flight recorder in the dedicated
SPIFFS partition. It retains current and previous rolling segments, CRC-protects every record,
samples changing identifiers at up to 5 Hz and stable identifiers at 1 Hz, and reports every
suppressed, queued, retained, dropped, and failed-write count. An encrypted BLE client can resume
chunked downloads without enabling Wi-Fi. See
[passive CAN flight recorder](docs/PASSIVE-CAN-FLIGHT-RECORDER.md).

The 4 MB partition table has two 1.5 MB OTA application slots and bootloader rollback
enabled. This is recovery groundwork, not a claim that a Wi-Fi OTA upload endpoint is
already implemented.

## Wi-Fi access-point status

Firmware `v0.1.0-dev.10` contains an authenticated, read-only commissioning surface but keeps it
**off by default**. A normal boot initializes neither Wi-Fi nor HTTP. The earlier unreleased
`v0.1.0-dev.6` bench behavior started an AP automatically; that policy was withdrawn after a Mac
joined the no-internet AP and left its normal network.

When deliberately enabled in a development build, the gateway advertises a WPA2 SoftAP named
`VHOS-STATUS-<chip-suffix>` for at most 15 minutes and serves `http://192.168.4.1/`. The HTTP
server accepts only authenticated `GET` requests for the page, its versioned JSON evidence, and a
health check. The per-device random password is created once, persisted in NVS, and printed only
on the physical UART commissioning console.

The page resolves its values from the same live BLE and TWAI/CAN state used by the gateway
transport. It reports firmware identity, uptime, reset reason, BLE link state, enforced
listen-only state, CAN counters, storage/power availability, and A/B OTA/rollback state.
It does not include configuration, reboot, erase, upload, diagnostic-command, or CAN-transmit
routes. When the 15-minute activated window ends, the HTTP server and Wi-Fi radio stop while BLE
and passive CAN observation continue.

Mac-presence detection is intentionally not used: Apple private addresses, sleep, missed radio
observations, or interference would make “not detected” an unsafe fail-open signal. The planned
production activation is an explicit user action over an already bonded and encrypted iPhone BLE
session.

The internet-hosted VHOS gateway provisioner remains a separate desktop USB flasher; it is
not served by the ESP32.

Design, evidence, security, and operator rationale are maintained alongside the target:

- [`docs/README.md`](docs/README.md) — documentation map and governing rules
- [`docs/SOFTAP-STATUS-ARCHITECTURE.md`](docs/SOFTAP-STATUS-ARCHITECTURE.md) — system boundaries and data lineage
- [`docs/SOFTAP-ACTIVATION-POLICY.md`](docs/SOFTAP-ACTIVATION-POLICY.md) — default-off policy, Mac isolation, and activation rationale
- [`docs/SOFTAP-STATUS-API.md`](docs/SOFTAP-STATUS-API.md) — route and field contract
- [`docs/SOFTAP-STATUS-SECURITY.md`](docs/SOFTAP-STATUS-SECURITY.md) — threat model and authority matrix
- [`docs/SOFTAP-STATUS-OPERATIONS.md`](docs/SOFTAP-STATUS-OPERATIONS.md) — commissioning and acceptance procedure
- [`docs/PASSIVE-CAN-DISCOVERY.md`](docs/PASSIVE-CAN-DISCOVERY.md) — safe bitrate probe, evidence, and limits
- [`docs/PASSIVE-CAN-FLIGHT-RECORDER.md`](docs/PASSIVE-CAN-FLIGHT-RECORDER.md) — persistent capture, BLE sync, record format, and low-trip workflow

## BLE commissioning

- Primary advertising includes the VHOS service UUID and short name; the full
  `VHOS-MRDIY-<chip-id>` name is in the scan response.
- Advertising and default connection transmit power are set to the classic ESP32's supported
  +9 dBm level.
- Evidence, health, and OTA notification subscriptions require an encrypted BLE link. On Apple
  platforms, subscribing initiates system pairing before the versioned handshake is sent.
- The gateway uses a random-static BLE identity persisted in NVS. Normal reboots, application-only
  flashes, and OTA preserve both that identity and the bond. A full NVS erase removes both, so the
  next boot creates a new identity and iOS treats the gateway as a new peripheral instead of
  repeatedly trying a stale key. Routine recovery therefore does not require **Forget This
  Device**. See [BLE bond-loss recovery](docs/BLE-BOND-LOSS-RECOVERY.md).
- Advertising uses that random-static identity directly. Resolvable-private address selection is
  intentionally disabled because it would hide the NVS identity epoch that tells Core Bluetooth
  the bond was erased.
- Boot evidence reports both the identity source (`generated` or `persisted`) and the real NimBLE
  bond-record counts. An identity is never used until it has been committed to NVS; failure to
  persist it leaves BLE unavailable rather than changing the address on every reboot.
- The NimBLE host uses an 8 KiB task stack. Encryption plus simultaneous evidence, health, and OTA
  subscriptions exceeded the ESP-IDF default 4 KiB stack during physical commissioning; the
  resulting watchdog reboot looked like a bond failure even though both bond records persisted.
- Large framed notifications are paced and briefly retry controller-buffer allocation, including
  while the connection is still using the 23-byte default ATT MTU.
- The peripheral requests a 30–50 ms connection interval, zero peripheral latency, and a six-second
  supervision timeout. Negotiated values are logged for physical reconnect diagnosis.
- Advertising recovery runs on the NimBLE event queue after a failed connection or disconnect;
  it does not create an unsupervised FreeRTOS retry task.

## Build

```bash
docker run --rm \
  -v "$PWD:/project" \
  -w /project/targets/mrdiy-esp32-v13 \
  espressif/idf:v5.5.3 \
  bash -lc 'idf.py set-target esp32 && idf.py build'
```

The repository's release tooling produces and validates the merged browser-flasher
image. Always retain a private full-flash backup before replacing factory firmware.
