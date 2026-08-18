# VHOS MrDIY ESP32 engineering documentation

This directory is the development record for the VHOS firmware that runs on the classic ESP32
with the MrDIY CAN Shield v1.3+. It supplements the target [README](../README.md) with the reasons,
security boundaries, contracts, and verification procedures behind the implementation.

## Document map

| Document | Purpose |
| --- | --- |
| [SoftAP status architecture](SOFTAP-STATUS-ARCHITECTURE.md) | Scope, requirements, lifecycle, components, data flow, and design decisions. |
| [SoftAP activation policy](SOFTAP-ACTIVATION-POLICY.md) | Why Wi-Fi is off by default, the Mac association incident, rejected proximity detection, and the explicit-activation state machine. |
| [SoftAP status API](SOFTAP-STATUS-API.md) | Browser routes, JSON contract, field lineage, nullability, caching, and compatibility policy. |
| [SoftAP security](SOFTAP-STATUS-SECURITY.md) | Threat model, authentication, credential lifecycle, attack-surface controls, and residual risks. |
| [SoftAP operations and verification](SOFTAP-STATUS-OPERATIONS.md) | Connection procedure, physical test plan, failure diagnosis, acceptance evidence, and recovery. |
| [BLE bond-loss recovery](BLE-BOND-LOSS-RECOVERY.md) | Persistent identity epochs, automatic recovery after NVS loss, failure behavior, and acceptance tests. |
| [BLE GATT schema migration](BLE-GATT-SCHEMA-MIGRATION.md) | One-time identity and bond migration after a service-database change, avoiding stale iOS GATT caches and manual device forgetting. |
| [Passive CAN discovery](PASSIVE-CAN-DISCOVERY.md) | Bounded 500/250-kbit listen-only probing, lock evidence, safety boundaries, and vehicle acceptance. |
| [Passive CAN flight recorder](PASSIVE-CAN-FLIGHT-RECORDER.md) | Offline capture, sampling, flash retention, record framing, resumable BLE transfer, iPhone storage, export, and field workflow. |
| [Authenticated temporary Wi-Fi OTA](AUTHENTICATED-WIFI-OTA.md) | Explicit encrypted-BLE activation, one-shot network credentials, signed image upload, A/B probationary boot, rollback, and acceptance gates. |
| [dev12 physical flash record](FIELD-FLASH-2026-08-17-DEV12.md) | Device identity, private recovery-backup checksum, NVS-preserving segment plan, boot evidence, and remaining physical gates. |
| [dev13 physical flash and GATT recovery record](FIELD-FLASH-2026-08-17-DEV13.md) | Signed segment flash, one-time identity migration, bond cleanup, independent macOS service enumeration, and remaining iPhone acceptance gate. |
| [dev14 physical flash and iPhone acceptance record](FIELD-FLASH-2026-08-17-DEV14.md) | Canonical device naming, recovery evidence, application-only flash boundary, boot proof, and real iPhone GATT/handshake/health acceptance. |
| [dev23 bonded-reconnect validation](FIELD-VALIDATION-2026-08-17-DEV23.md) | Dev21/dev22 failure evidence, NimBLE pre-CONNECT restoration ordering, exact dev23 build hash, Mac diagnostics, and physical restored-data plus saved-identity Disconnect/Reconnect acceptance. |
| [dev24 transport and GATT review closure](FIELD-VALIDATION-2026-08-17-DEV24.md) | Complete-frame session gating, strict handshake request validation, epoch-2/epoch-6 GATT compatibility audit, bond-preserving migration, exact build hash, and remaining physical gates. |
| [dev25 delivery-gate validation](FIELD-VALIDATION-2026-08-17-DEV25.md) | Bootstrap-only pre-session output, admission and delivery authorization, host-epoch RX/TX clearing, exact unsigned build identity, and remaining physical gates. |
| [dev26 deferred-control validation and physical acceptance](FIELD-VALIDATION-2026-08-17-DEV26.md) | Dev25 TX-stack failure, deferred initial health/status, exact build identity, saved-bond commissioning, sustained health, and automatic hard-reset recovery. |
| [dev28 capture-export recovery](FIELD-VALIDATION-2026-08-18-DEV28.md) | Dev27 stale-bond field recovery, reproduced bulk-transfer GATT stall, off-callback export worker architecture, and pre-dev29 acceptance boundary. |
| [dev29 capture retention and fault acceptance](FIELD-VALIDATION-2026-08-18-DEV29.md) | Header-only rotation protection, exact build identity, strict reset/app-death matrix, failure found in CoreBluetooth restoration, and remaining vehicle/power/export gates. |
| [dev30 passive J1979 evidence](FIELD-VALIDATION-2026-08-18-DEV30.md) | Passive Mode 01 response recognition, exact binary evidence contract, default-deny supported-PID request planner, build identity, and physical validation boundary. |

## Governing rule

The status surface is an observer. It may report facts already owned by firmware subsystems, but it
may not become a vehicle-control path. There is no HTTP endpoint for CAN transmission, OBD-II
requests, configuration mutation, credential disclosure, OTA upload, reboot, erase, or factory
reset.

Every field is either:

1. read directly from an authoritative local subsystem;
2. derived by a documented deterministic rule from those readings; or
3. explicitly unavailable as `null` with an availability flag or reason.

Zero is never substituted for an unknown measurement.

## Version scope

The authenticated status service implementation was introduced in the unreleased
`v0.1.0-dev.6` bench image. `v0.1.0-dev.7` changes the normal policy to **off by default** after
the Mac association test demonstrated that an automatically remembered no-internet AP is too
disruptive. The service remains a development and commissioning surface, not a production remote-
management interface. Later versions may add explicit encrypted-BLE activation or stronger
transport security, but they must retain the read-only and no-arbitrary-transmit boundaries.

`v0.1.0-dev.9` adds an NVS-persisted random-static BLE identity. If a full flash or NVS erase
removes the gateway bond, that same erase removes the identity epoch and the next boot presents a
new peripheral identity to iOS. This prevents an old iPhone bond from trapping commissioning in a
connect, encryption-failure, disconnect loop.

`v0.1.0-dev.10` adds a continuously bounded passive 500/250-kbit CAN probe. It can establish a
vehicle-bus candidate but cannot confirm OBD-II, issue diagnostic requests, or transmit a CAN
frame.

`v0.1.0-dev.11` adds a CRC-protected passive flight recorder and resumable encrypted-BLE log
transfer. The recorder is autonomous of the phone and its commands affect only local evidence
retention; the firmware still compiles no vehicle-bus transmit command.

`v0.1.0-dev.12` adds a separate temporary OTA service. It starts only after an encrypted BLE
owner request, accepts one bearer-authenticated signed application upload, writes only the inactive
A/B slot, and expires after five minutes. This does not add OTA authority to the read-only status
surface and does not change the default-off Wi-Fi policy.

`v0.1.0-dev.13` adds an NVS-persisted GATT schema version. A schema mismatch clears obsolete bond
and CCCD records and rotates the random-static identity exactly once, forcing iOS to discover the
new database automatically while preserving CAN captures and every unrelated storage namespace.

`v0.1.0-dev.14` adopts the product-wide `VHOS-4R-OBD-<MAC suffix>` display name. The label is
derived from silicon identity and does not replace the immutable gateway ID, rotate the persistent
BLE identity, or modify stored bonds and evidence lineage.

`v0.1.0-dev.23` preserves NimBLE bond restoration events that can precede GAP `CONNECT`. The
effective connect state is derived from the live connection descriptor plus restored CCCD events;
an already encrypted bonded link never starts another Security Manager procedure. Fresh links
remain bonded, encrypted, and Secure-Connections capable, and all outbound contracts use one
physical encrypted stream subscription.

`v0.1.0-dev.24` separates buffered transport progress from a completed application session. Only
a complete CRC-valid and allowlisted handshake request whose responses were queued opens live
health. It also records epoch `2` as explicitly bond-compatible with the attribute-identical epoch
`6`, allowing that audited migration to retain the BLE identity, security records, and CCCDs.

`v0.1.0-dev.25` applies the session contract to all outbound traffic. Only the handshake response
has a narrowly typed bootstrap scope. Its queue item is bound to the admission connection epoch,
and the TX task establishes readiness only after every notification chunk succeeds in that same
epoch. Initial health, live CAN, and every other frame require the established application session.
Queue admission and chunk delivery revalidate link state, and a NimBLE host reset clears
incremental receive state plus all pending transmit items.

`v0.1.0-dev.26` moves initial health/status construction off the 4 KB BLE TX stack after a physical
dev25 run proved a deterministic overflow. TX now only proves handshake delivery, transitions the
session, and wakes the existing 6 KB health/control task. That task publishes the session-required
initial frames and logs its stack high-water mark; the connection-epoch and authorization model is
unchanged. Physical USB-bench acceptance reused the saved bond, sustained recurring health for
more than two minutes, and automatically reconnected and verified dev26 after a deliberate Mac
chip-id hard reset, with no Pair/Forget/NVS erase. CAN remained at the expected zero-frame state
because the bench gateway was not connected to the vehicle.

`v0.1.0-dev.27` records a paired-once bond-policy epoch. If the gateway later has an empty NimBLE
key database while iOS still remembers the prior key, the firmware rotates its persisted BLE
identity once and establishes a new secure bond without asking the owner to use Settings.

`v0.1.0-dev.28` moves capture-file reads out of the NimBLE GATT callback into a depth-one,
generation-bound worker queue. Filesystem latency can no longer own the host event thread, and a
disconnect invalidates both queued requests and late results.

`v0.1.0-dev.29` preserves a nonempty previous capture when the current boot has no complete CAN
record. Its paired iOS `0.3.3 (9)` build and the exact dev29 firmware passed baseline plus three ESP
execution losses and three connected app-process deaths, with a new physical link, exact handshake,
and live health after every fault. Electrical rail loss and nonempty interrupted capture transfer
remain unclaimed physical gates.

`v0.1.0-dev.30` adds a passive J1979 evidence observer. It accepts only standard 11-bit ECU
responses `0x7E8`–`0x7EF`, ISO-TP single frames, and positive Mode 01 (`0x41`) payloads. The CAN
observer never blocks the capture task: it places recognized responses in a bounded queue and a
separate worker publishes the exact source sequence and monotonic timestamp over the established
BLE application session. A fixed `0x7DF` supported-PID request planner is unit-bounded by signed
plan, deterministic PARKED, idle capture, and confirmed protocol predicates. The production image
provides no caller or transmit executor, and TWAI remains listen-only, so active diagnostics remain
unavailable until a separately reviewed safety milestone.
