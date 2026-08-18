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
- No raw transmit, ELM327 command, or active diagnostic executor is compiled into this target.
  Dev30 contains only a fixed supported-PID request *planner* whose safety predicates default deny;
  no production caller can currently transmit its output.

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

The 4 MB partition table has two 1.5 MB OTA application slots and bootloader rollback enabled.
Firmware `v0.1.0-dev.12` adds an explicitly activated, five-minute, authenticated Wi-Fi upload to
the inactive slot with ESP-IDF native signed-application verification. Vehicle installation remains
blocked until motion and gateway supply are deterministically reported and the physical
upload/power-loss/rollback matrix passes. See
[authenticated temporary Wi-Fi OTA](docs/AUTHENTICATED-WIFI-OTA.md).

Firmware `v0.1.0-dev.13` versions the BLE GATT schema alongside the persisted random-static
identity. When a firmware update changes the service or characteristic database, the gateway clears
obsolete bond records and rotates its BLE identity once. iOS therefore performs fresh service
discovery automatically instead of trapping the app in `VALIDATING` or requiring the owner to use
“Forget This Device.” See [BLE GATT schema migration](docs/BLE-GATT-SCHEMA-MIGRATION.md).

Firmware `v0.1.0-dev.14` adopts the stable owner-facing name
`VHOS-4R-OBD-<MAC-suffix>`. It does not change the persisted BLE identity, bonds, complete
handshake `gateway_id`, or existing evidence lineage.

Firmware `v0.1.0-dev.23` makes bonded reconnect deterministic across NimBLE's legal
pre-`CONNECT` restoration ordering. Encryption, pairing completion, and the persisted stream CCCD
can be restored before the GAP connect callback. The callback preserves those facts, derives the
effective encryption state from the live connection descriptor, and skips a redundant security
procedure on an already encrypted bond. Fresh unencrypted links still initiate Secure Connections
pairing. Physical iPhone validation passed the restored data path and two explicit saved-identity
Reconnects without another Pair sheet; endurance and power/range recovery remain separate gates.
See [dev23 bonded-reconnect validation](docs/FIELD-VALIDATION-2026-08-17-DEV23.md).

Firmware `v0.1.0-dev.24` makes the application-session gate depend on a complete, CRC-valid
`gateway.handshake.request` whose handshake and initial health responses were queued. Buffering a
partial header or payload can no longer unlock periodic health. It also preserves identity, bond,
and stored CCCDs while migrating the independently audited, attribute-identical GATT epoch `2` to
epoch `6`; unknown or genuinely incompatible epochs retain the conservative rotation behavior.
See [dev24 transport and GATT review closure](docs/FIELD-VALIDATION-2026-08-17-DEV24.md).

Firmware `v0.1.0-dev.25` enforces that gate at outbound queue admission and again before every BLE
notification chunk. Before the application handshake completes, the only permitted outbound
frame is the typed bootstrap handshake response, and it can be queued only while the exact
connection is encrypted with its stream CCCD active. The TX task binds that response to its
admission connection epoch and opens the session only after every notification chunk returns
success in that same epoch. Initial and periodic health, live CAN, capture, and OTA are all
session-required. NimBLE host reset clears both incremental RX and queued TX state so no partial
command or frame crosses a host epoch. See
[dev25 delivery-gate validation](docs/FIELD-VALIDATION-2026-08-17-DEV25.md).

Firmware `v0.1.0-dev.26` keeps the BLE TX task limited to notification delivery and the atomic
session-state transition. Physical dev25 UART evidence showed that building initial health and OTA
status directly on its 4 KB stack overflowed immediately after readiness. Dev26 records a pending
control publish and wakes the existing 6 KB health task, which queues initial health/status and
reports its stack high-water mark. The same session and connection-epoch gates remain enforced.
Physical USB-bench acceptance then reused the saved iPhone bond, verified dev26, sustained health
for more than two minutes without a reboot, and automatically reconnected/reverified after a Mac
chip-id hard reset—without Pair, Forget, or NVS erase. Zero CAN frames are expected because the
bench gateway was not attached to the vehicle.
See [dev26 deferred-control validation](docs/FIELD-VALIDATION-2026-08-17-DEV26.md).

Firmware `v0.1.0-dev.27` adds an explicit paired-once bond-policy epoch. If a gateway that was
previously paired later boots with an empty NimBLE key database while iOS retains the old key, it
rotates its random-static identity once and allows a fresh secure bond without requiring the owner
to use **Forget This Device**.

Firmware `v0.1.0-dev.28` moves capture reads out of the NimBLE GATT callback. A depth-one queue and
dedicated 6 KiB worker perform SPIFFS access, and each request carries a transport generation so a
late result cannot cross a disconnect. See
[dev28 capture-export recovery](docs/FIELD-VALIDATION-2026-08-18-DEV28.md).

Firmware `v0.1.0-dev.29` prevents an empty/header-only current capture from replacing a nonempty
previous segment at boot. The exact dev29 build, paired with iOS `0.3.3 (9)`, passed a strict matrix
of three ESP execution losses and three connected app-process deaths; every cycle established a
new physical link, exact dev29 handshake, and live health without Pair, Forget, NVS erase, or manual
Connect. See
[dev29 capture retention and fault acceptance](docs/FIELD-VALIDATION-2026-08-18-DEV29.md).

Firmware `v0.1.0-dev.30` recognizes ISO 15765 single-frame positive Mode 01 responses on passive
11-bit CAN traffic at exactly 500 or 250 kbit/s, preserves their ECU, capture session, source sequence, and gateway monotonic
time, and emits a bounded versioned diagnostic-evidence record over the already authenticated BLE
stream. The phone and Android head unit enumerate PID `00/20/40/...` bitmaps independently for
each responding ECU and decode a pinned subset of standard values only after that ECU's
continuation chain is complete. The firmware still runs TWAI in listen-only mode and cannot issue
the request itself. A fixed functional-request planner exists for the future signed-plan,
deterministic-PARKED path, but has no production executor or permissive caller. See
[dev30 passive J1979 evidence](docs/FIELD-VALIDATION-2026-08-18-DEV30.md).

Firmware `v0.1.0-dev.31` closes the unsafe concurrency exposed by a real nonempty vehicle capture:
history reads and manual rotation are denied until the recorder is paused, its writer has drained,
and the file lock proves a quiescent snapshot. Periodic health no longer performs SPIFFS refresh
work, stopped export uses smaller 12-record chunks, and the handshake reports the reset reason for
phone-side failure attribution. The paired iOS 0.3.5 build inventories active recordings without
bulk downloading them. See
[dev31 live-capture/export isolation](docs/FIELD-VALIDATION-2026-08-18-DEV31.md).

Firmware `v0.1.0-dev.32` separates controller receive, observer fan-out, and flash-retention
quality. A priority-12 task drains a 512-entry TWAI queue in bounded batches and hands complete
timestamped observations to a separate 256-entry queue. Capture sampling, passive J1979 parsing,
and live BLE display run on the lower-priority dispatch task rather than in the critical receive
loop. Health now reports TWAI missed/overrun counts, both queue depths and capacities, observer
high-water/drop counts, and the existing recorder sampling/storage counters independently. A
history-transfer pause is flushed and automatically resumed if the BLE session ends. See
[dev32 acquisition-quality foundation](docs/FIELD-VALIDATION-2026-08-18-DEV32.md).

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
- [`docs/AUTHENTICATED-WIFI-OTA.md`](docs/AUTHENTICATED-WIFI-OTA.md) — encrypted-BLE activation, temporary network, signed image, A/B rollback, and physical acceptance

## BLE commissioning

- Primary advertising includes the VHOS service UUID and short name; the full
  `VHOS-4R-OBD-<MAC-suffix>` name is in the scan response. The canonical name is a stable
  owner-facing alias; the complete handshake `gateway_id` remains the immutable evidence identity.
- Advertising and default connection transmit power are set to the classic ESP32's supported
  +9 dBm level.
- Evidence, health, and OTA are self-describing framed messages multiplexed over one encrypted
  stream notification characteristic and one physical CCCD. On a fresh Apple connection, the
  gateway proactively initiates Secure Connections pairing before the versioned handshake. A
  bonded reconnect restores encryption and the stream CCCD without another owner prompt.
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
- The NimBLE host uses an 8 KiB task stack. Before outbound frames were multiplexed over one stream
  CCCD, encryption plus simultaneous evidence, health, and OTA subscriptions exceeded the ESP-IDF
  default 4 KiB stack during physical commissioning; the resulting watchdog reboot looked like a
  bond failure even though both bond records persisted.
- Large framed notifications are paced and briefly retry controller-buffer allocation, including
  while the connection is still using the 23-byte default ATT MTU.
- The peripheral requests a 30–50 ms connection interval, zero peripheral latency, and a six-second
  supervision timeout. Negotiated values are logged for physical reconnect diagnosis.
- Advertising recovery runs on the NimBLE event queue after a failed connection or disconnect;
  it does not create an unsupervised FreeRTOS retry task.
- NimBLE may emit pairing, encryption, and `reason=bond-restore` subscription events before GAP
  `CONNECT`. The connect callback preserves those restored flags. It never starts a second security
  procedure when the live descriptor already reports `encrypted=1`; disconnect, host reset, and
  cold boot remain the only normal boundaries that clear in-memory subscription state.
- Application readiness is separate from ATT write success. A fragmented command remains pending
  until the complete frame passes both CRCs, the request contract and version are allowlisted, and
  every handshake-response notification chunk succeeds in the same connection epoch.
- The outbound queue carries an explicit scope and its admission connection epoch. Only the
  correctly typed handshake bootstrap frame may bypass the session gate; initial health and every
  other frame require connection, encryption, stream subscription, and application readiness at
  both enqueue and delivery time.

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
