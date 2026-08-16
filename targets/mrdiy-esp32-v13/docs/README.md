# VHOS MrDIY ESP32 engineering documentation

This directory is the development record for the VHOS firmware that runs on the classic ESP32
with the MrDIY CAN Shield v1.3+. It supplements the target [README](../README.md) with the reasons,
security boundaries, contracts, and verification procedures behind the implementation.

## Document map

| Document | Purpose |
| --- | --- |
| [SoftAP status architecture](SOFTAP-STATUS-ARCHITECTURE.md) | Scope, requirements, lifecycle, components, data flow, and design decisions. |
| [SoftAP status API](SOFTAP-STATUS-API.md) | Browser routes, JSON contract, field lineage, nullability, caching, and compatibility policy. |
| [SoftAP security](SOFTAP-STATUS-SECURITY.md) | Threat model, authentication, credential lifecycle, attack-surface controls, and residual risks. |
| [SoftAP operations and verification](SOFTAP-STATUS-OPERATIONS.md) | Connection procedure, physical test plan, failure diagnosis, acceptance evidence, and recovery. |

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

The authenticated status service is introduced for the `v0.1.0-dev.6` milestone. It remains a
development and commissioning surface, not a production remote-management interface. Later
versions may change the enrollment flow or add transport security, but they must retain the
read-only and no-arbitrary-transmit boundaries.
