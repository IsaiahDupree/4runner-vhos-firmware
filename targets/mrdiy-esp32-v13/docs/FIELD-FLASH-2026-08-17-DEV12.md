# MrDIY ESP32 physical flash record — v0.1.0-dev.12

- Status: physical USB flash and boot verification passed on 2026-08-17.
- Target: MrDIY ESP32 CAN Shield v1.3+ gateway.
- Release: `v0.1.0-dev.12`.

## Device identity

The Mac detected one USB serial device. A read-only ESP ROM query identified:

- chip: ESP32-D0WDQ6 revision 1.1;
- flash size: 4 MiB;
- Wi-Fi station MAC: `94:54:c5:b0:8d:14`;
- VHOS gateway ID: `esp32-9454c5b08d14`;
- advertised name after boot: `VHOS-MRDIY-B08D14`.

The MAC suffix matches the OBD/CAN gateway previously observed by the iPhone. No second ESP32
serial device was present during this operation.

## Recovery backup

A complete 4 MiB flash backup was captured before any write:

```text
/Users/isaiahdupree/.local/share/vhos/device-backups/esp32-9454c5b08d14-pre-dev12-20260817.bin
SHA-256: 67584837e6c451914c52e1ed4baf1394b684a9b1975213739d2e35f04456f55b
```

The backup is intentionally outside the public repository because it may contain device-local NVS
and Bluetooth security material.

## Flash plan

The merged image was not written at offset zero. Writing the merged file would erase gap sectors,
including the NVS range. Instead, the release segments were written individually:

| Address | Segment | Result |
|---:|---|---|
| `0x1000` | signed-update-aware bootloader | write hash verified |
| `0x8000` | A/B partition table | write hash verified |
| `0xD000` | initial OTA selection data | write hash verified |
| `0x10000` | ECDSA-signed VHOS application | write hash verified |

The device NVS range `0x9000–0xCFFF` was not part of the write plan. The persisted random BLE
identity remained `e3:2d:bd:5e:5d:ed` after reboot. The firmware reported zero local and peer
NimBLE security records, so a fresh encrypted pairing may occur; this is not evidence that the
stable gateway identity was lost.

## Boot evidence

The physical UART boot log established:

```text
Project name:     vhos_mrdiy_esp32_v13
App version:      0.1.0-dev.12
ESP-IDF:          v5.5.3
PASSIVE_CAN_READY mode=listen-only initial_bitrate=500000 rx_gpio=4 tx_gpio=5
VHOS_BLE_ADVERTISING name=VHOS-MRDIY-B08D14
VHOS_SOFTAP_DISABLED reason=default-safe-policy activation=encrypted-ble-pending
VHOS_SELF_TEST_PASS ... vehicle_bus_read_only=true
```

The gateway was on the USB bench rather than the vehicle bus, so the passive bitrate probe later
moved from 500 kbit/s to 250 kbit/s after observing no valid frames. That is expected and is not an
OBD protocol result.

## Boundaries and next acceptance gate

This record proves backup, segment flashing, application boot, BLE advertisement startup,
listen-only CAN startup, and default-off SoftAP behavior on the physical gateway. It does not yet
prove:

- physical A/B rollback restoration;
- iPhone Hotspot Configuration provisioning;
- an authenticated iPhone-to-ESP32 OTA transfer;
- deterministic parked-state or supply-voltage evidence;
- vehicle-bus reception after dev12 installation.

Those items require the matching iPhone build and a subsequent controlled vehicle session.
