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

## First milestone

The image exposes the VHOS BLE service used by the iOS app and reports actual
gateway health: received frames, controller drops, bus errors, bus-off transitions,
and the enforced listen-only state. Supply voltage, motion, capture storage, protocol
confirmation, active OBD queries, and Wi-Fi OTA remain unavailable until their real
implementations land; the app therefore shows those states as unavailable or pending.

The 4 MB partition table has two 1.5 MB OTA application slots and bootloader rollback
enabled. This is recovery groundwork, not a claim that a Wi-Fi OTA upload endpoint is
already implemented.

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
