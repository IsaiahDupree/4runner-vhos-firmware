# MrDIY ESP32 physical flash and iPhone acceptance — v0.1.0-dev.14

Date: 2026-08-17  
Status: physical USB installation, boot verification, and iPhone end-to-end acceptance passed

## Purpose

Dev14 replaces the development label `VHOS-MRDIY-<suffix>` with the product-wide canonical OBD
gateway name `VHOS-4R-OBD-<MAC suffix>`. The suffix remains tied to silicon identity; changing the
human-readable label does not rotate the persisted BLE address, erase bonds, or change the immutable
`gateway_id` used by evidence records.

The matching iPhone build also normalizes the legacy label and never presents Apple's per-peripheral
CoreBluetooth UUID as physical device identity.

## Positively identified target

Only one serial device was present before the write. ESP ROM queries established:

| Field | Observed value |
| --- | --- |
| USB serial port | `/dev/cu.usbserial-0001` |
| Chip | ESP32-D0WDQ6 revision 1.1 |
| Silicon MAC | `94:54:c5:b0:8d:14` |
| Flash | 4 MiB, 3.3 V |
| Immutable gateway ID | `esp32-9454c5b08d14` |
| Canonical display name | `VHOS-4R-OBD-B08D14` |

The MAC and documented USB mapping proved this was the OBD/CAN gateway rather than the separate A/C
sensor node.

## Recovery evidence

The previously verified private 4 MiB recovery image remains available at:

```text
/Users/isaiahdupree/.local/share/vhos/device-backups/esp32-9454c5b08d14-pre-dev12-20260817.bin
SHA-256: 67584837e6c451914c52e1ed4baf1394b684a9b1975213739d2e35f04456f55b
```

Two attempts to take a new full-flash backup ended before completion because the USB serial stream
stopped at 78% and 9%. A conservative ROM-loader attempt later ended at 39.6% when the USB device
disappeared from macOS. All three operations were reads; no flash sector had been modified.

After the cable was reseated, the device identity was queried again and matched. The state that had
changed since the prior full backup was then captured separately before the application write:

| Private artifact | Address / size | SHA-256 |
| --- | --- | --- |
| `esp32-9454c5b08d14-pre-dev14-nvs-20260817.bin` | `0x9000` / 16,384 bytes | `b921765fc426295352cc5d4403b438229c1c2194414208ffc3a8e17962097cd2` |
| `esp32-9454c5b08d14-pre-dev14-otadata-20260817.bin` | `0xD000` / 8,192 bytes | `8ba3b110139f45443d4f268d1a3373ef99a1718b71d51664531b83ee2d4b91a3` |

These private files preserve the current random-static BLE identity, bond store, GATT schema epoch,
and OTA selection state. They are intentionally excluded from the public repository.

## Installed artifact and write boundary

| Field | Value |
| --- | --- |
| Source commit | `d7ccfb45c178f1f88f4d7244e26c522cb878737b` |
| Application version | `0.1.0-dev.14` |
| Signed OTA image | `vhos-mrdiy-esp32-v13-v0.1.0-dev.14-ota.bin` |
| Image size | 1,048,564 bytes |
| Image SHA-256 | `2a75404c4b3db801e7714625a02a791ed3abfc70ebe18e6dcb2b8b8a11a81144` |
| Flash address | `0x10000` (`ota_0`) |
| esptool result | data hash verified |

Only `0x10000–0x10FFFF` was erased for the signed application write. The bootloader, partition table,
NVS, OTA metadata, inactive A/B slot, and `storage` capture partition were not written or erased.

## Boot acceptance

A controlled hardware reset produced the following authoritative evidence:

```text
App version:      0.1.0-dev.14
Loaded app from partition at offset 0x10000
PASSIVE_CAN_READY mode=listen-only initial_bitrate=500000
BLE_BOND_STORE our_security_records=1 peer_security_records=1
BLE_IDENTITY_READY type=random-static source=persisted gatt_schema=2 address=fc:41:4e:b7:e8:6c
VHOS_BLE_ADVERTISING name=VHOS-4R-OBD-B08D14 short_name=VHOS
VHOS_SOFTAP_DISABLED reason=default-safe-policy activation=encrypted-ble-pending
VHOS_SELF_TEST_PASS firmware=0.1.0-dev.14 gateway=esp32-9454c5b08d14 vehicle_bus_read_only=true
```

This proves that the canonical label changed while the persistent BLE identity and both security
records survived the application-only update.

## Physical iPhone acceptance

The iPhone app at source commit `01fbc8982c1ffbaceefa3ca97295b09a7e116265` was signed, installed,
and launched on Isaiah's iPhone. The commissioning launch used the real Core Bluetooth stack and no
simulator data.

The first advertisement was weak (`-93 dBm`) and Core Bluetooth initially returned its cached legacy
`peripheral.name`. Two encrypted connection attempts timed out. The application's normal automatic
reconnect path succeeded on attempt three without deleting the iOS bond or using **Forget This
Device**:

```text
LINK_CONNECTED
SERVICES_DISCOVERED uuids=33613EB3-FFCA-42D1-83FA-A18F12B3F123
CHARACTERISTICS_DISCOVERED service=33613EB3-FFCA-42D1-83FA-A18F12B3F123 count=4
HANDSHAKE_VERIFIED firmware=0.1.0-dev.14
HEALTH_DECODED scan=PROBING_500K bitrate=500000 controller=true frames=0 errors=0 bus_off=0
CAPTURE_SYNC_COMPLETE downloaded=0 local_sessions=2
```

The zero-frame result is expected on the USB bench and is not interpreted as an OBD-II failure. It
proves transport, service discovery, secure characteristic access, handshake decoding, live health,
and capture-index synchronization. The app was relaunched normally after the trace was collected.

## Result

The physical system now has a stable three-layer identity:

1. owner-facing label: `VHOS-4R-OBD-B08D14`;
2. immutable evidence identity: `esp32-9454c5b08d14`;
3. private transport identity: persisted random-static BLE address plus Apple's internal
   CoreBluetooth identifier.

Only the first is displayed as the device name. Transport identifiers remain internal and may not
be used as product identity.
