# MrDIY ESP32 physical flash record — v0.1.0-dev.13

Date: 2026-08-17  
Status: firmware and macOS GATT acceptance passed; final iPhone application acceptance pending

## Purpose

The iPhone repeatedly reached a physical BLE connection but remained at `VALIDATING`, with the
versioned VHOS service reported as `NOT FOUND`. Dev12 had added an OTA-status characteristic while
retaining the previously bonded random-static BLE identity. This run verifies the dev13 GATT-schema
migration and establishes whether the service database itself is accessible.

## Device identity and recovery baseline

| Field | Value |
| --- | --- |
| Hardware | classic ESP32-D0WDQ6 revision 1.1 + MrDIY CAN Shield v1.3+ |
| Silicon MAC | `94:54:c5:b0:8d:14` |
| USB port | `/dev/cu.usbserial-0001` during this run |
| Flash capacity | 4 MiB |
| Private pre-dev12 full-flash backup | `esp32-9454c5b08d14-pre-dev12-20260817.bin` |
| Backup SHA-256 | `67584837e6c451914c52e1ed4baf1394b684a9b1975213739d2e35f04456f55b` |
| Backup location policy | owner-local private storage; never published |

The private ECDSA signing key is also kept outside the public repository. The public verification
key is compiled into firmware, and the private key is available to CI only through an encrypted
repository secret.

## Pre-flash evidence

Dev12 boot evidence immediately before the fix reported:

```text
App version: 0.1.0-dev.12
BLE_BOND_STORE our_security_records=1 peer_security_records=1
BLE_IDENTITY_READY type=random-static source=persisted address=e3:2d:bd:5e:5d:ed
VHOS_SOFTAP_DISABLED
VHOS_SELF_TEST_PASS
```

The iPhone screenshot simultaneously showed a healthy RSSI and connected/validating physical link,
but no VHOS service. The application and firmware UUID byte sequences were compared and matched;
the failure was not caused by a mismatched UUID constant.

## Build and signature evidence

- Source commit: `f82f8b93926ddfbb1869f8928761a5f188dec971`.
- Release: `v0.1.0-dev.13`.
- ESP-IDF: `v5.5.3`.
- Chip target: classic `ESP32`.
- Application size: `1,048,564` bytes after the ECDSA V1 signature block.
- Signed application SHA-256: `2d5aa02c8b32ced449829959f10347dac10257475fe8c4acf6a27bbd0f8c553f`.
- Merged image SHA-256: `d12241e3bc05ed3b4a3a9e3bd84be5d51259d1096b46502dc4eb75af73c0eba1`.
- Signature verification: passed with `espsecure.py verify_signature`.
- Automated release validation: all static safety, partition, signature, and byte-exact merge checks
  passed.
- GitHub Actions build for the source commit: passed.

## NVS-preserving flash plan

Only the components affected by the signing-key and application update were written:

| Offset | Segment | Result |
| --- | --- | --- |
| `0x1000` | bootloader | write completed; flash hash verified |
| `0x10000` | ECDSA-signed dev13 application | write completed; flash hash verified |

The following were not erased or written:

- NVS at `0x9000`;
- OTA selection data at `0xD000`;
- partition table at `0x8000`;
- passive capture storage at `0x310000`;
- inactive OTA application slot.

Preserving NVS was essential: the firmware needed to observe the old identity with no stored schema
version so it could exercise the migration path rather than behave like a factory-new device.

## Post-flash boot evidence

A follow-up boot after the one-time migration reported:

```text
App version: 0.1.0-dev.13
BLE_BOND_STORE our_security_records=0 peer_security_records=0
BLE_IDENTITY_READY type=random-static source=persisted gatt_schema=2 address=fc:41:4e:b7:e8:6c
VHOS_BLE_ADVERTISING name=VHOS-MRDIY-B08D14
VHOS_SOFTAP_DISABLED reason=default-safe-policy activation=encrypted-ble-pending
VHOS_SELF_TEST_PASS firmware=0.1.0-dev.13 vehicle_bus_read_only=true
```

The durable before/after evidence establishes that the identity changed from
`e3:2d:bd:5e:5d:ed` to `fc:41:4e:b7:e8:6c`, schema `2` was persisted, and the stale local/peer bond
records were removed. The next reboot reused the new identity, proving that rotation is one-time and
not a boot loop.

## Independent macOS BLE acceptance

A macOS Core Bluetooth scan observed:

| Evidence | Result |
| --- | --- |
| Local name | `VHOS-MRDIY-B08D14` |
| RSSI | `-40 dBm` during the scan |
| Advertised service | `33613EB3-FFCA-42D1-83FA-A18F12B3F123` |
| GATT connection | passed |

Service enumeration returned all required characteristics:

| UUID | Required properties | Observed |
| --- | --- | --- |
| `B3D3279B-0244-4D54-A2AB-A1AB47A5FC0A` | write, write without response | passed |
| `265B90C0-A600-4659-BBBD-5CDA411C49CC` | read, notify | passed |
| `BCB5699A-A9B4-49B8-B69B-D2DFF19B41A9` | read, notify | passed |
| `18D21F8E-D190-4DB3-923C-27BBFC355874` | read, notify | passed |

This proves the versioned VHOS service database is present and discoverable independently of the
iPhone application's restored Core Bluetooth state.

## Published recovery paths

- Public source and signed release:
  `https://github.com/IsaiahDupree/4runner-vhos-firmware/releases/tag/v0.1.0-dev.13`
- Public backup-first USB flasher:
  `https://vhos-gateway-provisioner.isaiahdupree.chatgpt.site`

The public flasher serves a same-origin manifest and byte-exact dev13 images. It still requires an
explicit hardware selection and full-flash backup before install.

## Remaining acceptance gate

On the iPhone, the owner should tap `Disconnect` and then `Scan for gateway` once if the currently
installed app is still holding the old in-memory candidate. No Settings-level “Forget This Device”
step is required. Acceptance is complete when the iPhone reports:

1. VHOS service found;
2. all four characteristics discovered;
3. encrypted notification subscriptions active;
4. dev13 handshake verified;
5. live health frames arriving;
6. no reconnect/validating cycle across a normal gateway reboot.

The current app source also retires a stale GATT candidate and resumes service-filtered scanning
automatically after any service/characteristic discovery failure or timeout. That app change has
passed all 22 portable Swift tests and the complete iOS simulator build.
