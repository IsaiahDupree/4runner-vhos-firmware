# VHOS target: ESP32 + MrDIY CAN Shield v1.3+

This target is for the physically verified classic ESP32-D0WDQ6 board running the
MrDIY CAN Shield v1.3+ pinout. It is not compatible with the WiCAN Pro ESP32-S3
image.

## Hardware contract

- MCU: classic ESP32, 4 MB flash
- CAN RX: GPIO 4
- CAN TX: GPIO 5
- CAN controller mode: forced `TWAI_MODE_LISTEN_ONLY`
- Initial passive bus timing: 500 kbit/s, matching the recovered factory image
- No raw transmit, ELM327 command, or active diagnostic path is compiled into this target

The pin assignment and 500 kbit/s factory default are traceable to the recovered
device image and the upstream MrDIY CANaBus v1.3+ firmware. They do not, by
themselves, prove a vehicle protocol.

## Gateway health foundation

The image exposes the VHOS BLE service used by the iOS app and reports actual
gateway health: received frames, controller drops, bus errors, bus-off transitions,
and the enforced listen-only state. Supply voltage, motion, capture storage, protocol
confirmation, active OBD queries, and Wi-Fi OTA upload remain unavailable until their real
implementations land; the app therefore shows those states as unavailable or pending.

The 4 MB partition table has two 1.5 MB OTA application slots and bootloader rollback
enabled. This is recovery groundwork, not a claim that a Wi-Fi OTA upload endpoint is
already implemented.

## Wi-Fi access-point status

Firmware `v0.1.0-dev.6` adds an authenticated, read-only commissioning surface at
`http://192.168.4.1/`. During the first 15 minutes after boot, the gateway advertises a
WPA2 SoftAP named `VHOS-STATUS-<chip-suffix>`. The HTTP server accepts only authenticated
`GET` requests for the page, its versioned JSON evidence, and a health check. The per-device
random password is created once, persisted in NVS, and printed only on the physical UART
commissioning console.

The page resolves its values from the same live BLE and TWAI/CAN state used by the gateway
transport. It reports firmware identity, uptime, reset reason, BLE link state, enforced
listen-only state, CAN counters, storage/power availability, and A/B OTA/rollback state.
It does not include configuration, reboot, erase, upload, diagnostic-command, or CAN-transmit
routes. When the 15-minute boot window ends, the HTTP server and Wi-Fi radio stop while BLE
and passive CAN observation continue.

The internet-hosted VHOS gateway provisioner remains a separate desktop USB flasher; it is
not served by the ESP32.

Design, evidence, security, and operator rationale are maintained alongside the target:

- [`docs/README.md`](docs/README.md) — documentation map and governing rules
- [`docs/SOFTAP-STATUS-ARCHITECTURE.md`](docs/SOFTAP-STATUS-ARCHITECTURE.md) — system boundaries and data lineage
- [`docs/SOFTAP-STATUS-API.md`](docs/SOFTAP-STATUS-API.md) — route and field contract
- [`docs/SOFTAP-STATUS-SECURITY.md`](docs/SOFTAP-STATUS-SECURITY.md) — threat model and authority matrix
- [`docs/SOFTAP-STATUS-OPERATIONS.md`](docs/SOFTAP-STATUS-OPERATIONS.md) — commissioning and acceptance procedure

## BLE commissioning

- Primary advertising includes the VHOS service UUID and short name; the full
  `VHOS-MRDIY-<chip-id>` name is in the scan response.
- Advertising and default connection transmit power are set to the classic ESP32's supported
  +9 dBm level.
- Evidence, health, and OTA notification subscriptions require an encrypted BLE link. On Apple
  platforms, subscribing initiates system pairing before the versioned handshake is sent.
- Boot evidence reports the real NimBLE bond-record counts. The firmware does not silently erase
  bonds; if either side has a stale development key, forget VHOS on the phone and clear only the
  backed-up gateway NVS partition before pairing again.
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
