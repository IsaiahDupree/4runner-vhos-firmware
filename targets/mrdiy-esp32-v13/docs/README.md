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
