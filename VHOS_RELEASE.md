# VHOS firmware release process

This fork builds the WiCAN Pro ESP32-S3 target from the upstream `v4.50p`
baseline. ESP-IDF `v5.5.3` is used because that is the exact version recorded in
the upstream dependency lock at the pinned tag.

The MeatPi CH34x/CH343 USB driver is retained at the exact upstream commit as a
Git submodule. This bypasses a stale Component Manager checksum in the upstream
lockfile without altering or silently upgrading the driver source.

## Safety boundary in `v0.1.0-dev.1`

- CAN is forced to silent/listen-only mode at boot.
- The generic CAN transmit function rejects every request in a VHOS build.
- Wi-Fi/TCP and raw BLE input queues cannot reach the vehicle-bus command
  parser.
- AutoPID discovery and its HTTP test endpoints are not registered.
- The web ELM327 terminal rejects transmit access.
- The public BLE contract accepts only a framed VHOS handshake request. Every
  other command type is denied.
- A pending OTA image is confirmed only after the firmware sees both A/B app
  slots, listen-only CAN state, and initialized BLE state. A failed self-test
  requests the ESP-IDF rollback path.

This release does not advertise signed-image OTA or active protocol discovery.
Those capabilities remain locked in the iPhone app until their implementation
and physical recovery tests pass.

## Reproducible build

From the repository root with Docker available:

```bash
docker run --rm -v "$PWD:/project" -w /project \
  espressif/idf:v5.5.3 \
  bash -lc '. /opt/esp/idf/export.sh >/dev/null && bash tools/build_vhos_release.sh v0.1.0-dev.1'
```

The script builds the app, creates a single address-zero merged image, parses
the generated binary partition table, verifies the embedded bootloader,
partition table, OTA metadata, and application segments byte-for-byte, and
writes SHA-256 release metadata under `dist/`.

The merged image is for the backup-first address-zero Web Serial install. The
separate `-ota.bin` application image is published for the future signed Wi-Fi
OTA path, but this development release does not authorize or advertise that
capability yet.

## Recovery test status

The automated check is a static merged-image and rollback-topology test. The
physical gate is separate and cannot pass without the target board:

1. Read and retain the entire factory flash.
2. Flash the merged development image.
3. Confirm the iPhone receives a valid VHOS handshake over encrypted BLE.
4. Install an intentionally unconfirmed test image into the inactive OTA slot.
5. Confirm the bootloader returns to the prior valid slot.
6. Restore the factory backup and verify its full-flash SHA-256.

Until all six physical steps are recorded against an identified board, the
release remains `development` and bench-only.
