# Authenticated temporary Wi-Fi OTA

Status: implemented in `v0.1.0-dev.12`; automated build acceptance complete; physical iPhone
upload, probationary boot, forced-failure rollback, and vehicle power-loss testing pending

## Outcome

The normal gateway boot path keeps Wi-Fi and HTTP off. An owner-approved iPhone update may open a
hidden, one-client WPA2 access point for at most five minutes, upload one pre-approved signed ESP-IDF
application image to the inactive A/B slot, and reboot into ESP-IDF probationary validation. The
temporary network is not a general web page, status server, file browser, or vehicle command path.

This exists to reduce return trips to the vehicle after the one-time USB bootstrap. USB recovery and
a private full-flash backup remain mandatory until the physical rollback matrix passes.

## Trust layers

The update uses two independent signatures:

1. The iPhone verifies the `.vhosota` distribution container with an Ed25519 release key. That
   signature binds the canonical manifest to the exact application bytes.
2. `esp_ota_end()` verifies the ESP-IDF ECDSA V1 signature appended to the application image against
   the public key compiled into this target's bootloader/application configuration.

The development target uses `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`. It rejects unsigned or
wrong-key OTA images without burning irreversible Secure Boot eFuses. The private ECDSA key is stored
outside the repository. The repository contains only its 64-byte public verification material.

This is intentionally not yet a claim of production key custody. Production key generation,
offline storage, rotation, revocation, and recovery require a separate accepted procedure.

## Authority and safety boundary

The OTA service can:

- pause and flush passive flash logging;
- initialize a temporary isolated Wi-Fi access point;
- accept one authenticated HTTP upload;
- write the inactive application partition;
- request probationary boot of a cryptographically accepted image; and
- persist and report the update outcome.

It cannot:

- transmit CAN, K-line, or J1850 traffic;
- issue an OBD diagnostic request;
- clear codes or invoke an Active Test;
- write the running application slot;
- erase NVS, the BLE identity epoch, capture storage, or the full flash;
- select an arbitrary partition or URL supplied by HTTP;
- provide internet routing; or
- start automatically at boot.

The iPhone preflight still requires deterministic `PARKED`, sufficient reported gateway supply,
listen-only agreement, a flushed inactive capture, exact hardware compatibility, required
capabilities, a non-downgrade version, and explicit owner confirmation. The current firmware reports
vehicle motion and supply as unavailable, so real installation remains visibly blocked until those
two measurements are implemented. That gate must not be replaced with guessed motion or voltage.

## Activation sequence

```text
normal boot
  -> BLE + passive CAN + recorder active
  -> Wi-Fi OFF
  -> iPhone verifies .vhosota Ed25519 signature and manifest
  -> iPhone requests recorder pause; index + health confirm flushed/inactive
  -> iPhone preflight proves PARKED, supply, compatibility, listen-only, capabilities
  -> encrypted BLE message 8: gateway.ota-control-request / ACTIVATE
  -> gateway rechecks encrypted BLE and running listen-only CAN controller
  -> gateway generates random SSID suffix, 20-character WPA2 passphrase, 40-character bearer token
  -> hidden WPA2 AP, PMF required, one station, five-minute lease
  -> encrypted OTA-status notification returns credentials and fixed local upload URL
  -> iOS joins with NEHotspotConfiguration(joinOnce=true)
  -> POST /api/v1/ota/image with Bearer token and exact Content-Length
  -> streaming SHA-256 + inactive-partition write
  -> SHA-256 equals owner-approved manifest
  -> esp_ota_end verifies native ESP-IDF ECDSA signature
  -> boot partition changes to inactive slot
  -> PENDING_REBOOT outcome persisted
  -> HTTP success response
  -> reboot into probationary image
  -> startup verifies NVS, capture storage, listen-only CAN, BLE, and transport readiness
  -> esp_ota_mark_app_valid_cancel_rollback
  -> POST_PASSED outcome reported after BLE reconnect
```

Any activation, network, HTTP, receive, size, hash, signature, or partition error shuts the temporary
service down and resumes passive capture. An expired lease also shuts it down and resumes capture.

## BLE contracts

The command is a normal CRC32C-protected VHOS frame with message type `8`. Its JSON payload is:

```json
{
  "approved_at": "2026-08-17T00:00:00Z",
  "contract": "gateway.ota-control-request",
  "contract_version": "1.0.0",
  "firmware_sha256": "<64 lowercase or uppercase hex characters>",
  "firmware_size_bytes": 1049008,
  "firmware_version": "0.1.0-dev.12",
  "operation": "ACTIVATE",
  "package_id": "<UUID>"
}
```

`CANCEL` uses the same identity fields so logs can retain which approved package was abandoned.
The command characteristic requires encryption. Firmware does not accept SSID, passphrase, token,
partition, HTTP path, or arbitrary firmware bytes in this request.

The dedicated encrypted OTA notification characteristic returns:

```json
{
  "bearer_token": "<only present while NETWORK_READY>",
  "contract": "gateway.ota-status",
  "contract_version": "1.0.0",
  "detail": "Temporary authenticated OTA network is ready.",
  "expires_in_seconds": 300,
  "firmware_version": "0.1.0-dev.12",
  "gateway_id": "esp32-...",
  "maximum_image_bytes": 1572864,
  "package_id": "<UUID>",
  "passphrase": "<only present while NETWORK_READY>",
  "session_active": true,
  "ssid": "VHOS-OTA-<chip-suffix>",
  "state": "NETWORK_READY",
  "upload_url": "http://192.168.4.1/api/v1/ota/image"
}
```

Credentials are omitted from all later status frames and are zeroed in RAM when the service ends.
They are not persisted to NVS and are not printed to UART. The SSID is not a credential and may be
logged for commissioning diagnosis.

Status values currently emitted or persisted are `NETWORK_READY`, `UPLOADING`, `FAILED`, `EXPIRED`,
`CANCELLED`, `PREPARED`, `REBOOTING`, `POST_PASSED`, `ROLLED_BACK`, `NOT_ACTIVATED`, and
`BOOT_SELECTION_FAILED`.

## HTTP contract

There is exactly one route:

```http
POST /api/v1/ota/image HTTP/1.1
Authorization: Bearer <40-character session token>
Content-Type: application/octet-stream
Content-Length: <exact approved application size>
```

The server rejects missing/incorrect authorization, concurrent uploads, expired leases, zero or
unexpected lengths, oversized images, missing/inappropriate inactive slots, interrupted transfer,
SHA-256 mismatch, native signature failure, and boot-selection failure. It sets `Cache-Control:
no-store`, `X-Content-Type-Options: nosniff`, and closes the response connection.

HTTP is used only inside the WPA2-protected, isolated local link. The bearer token is an independent
application credential, and the image is independently authenticated twice. HTTPS would add a
certificate bootstrap and memory cost but would not replace those controls. Production threat
review may still require TLS or a proof-of-possession channel.

## Network behavior and the earlier Wi-Fi incident

The OTA AP is separate from the read-only status AP. Neither starts automatically. The iPhone joins
the OTA AP only after a user-approved update; its `joinOnce` configuration is removed immediately
after success or failure. During that short interval the iPhone may leave its normal Wi-Fi and use
cellular for internet, because the gateway intentionally provides no upstream route.

Mac-presence detection remains rejected. Missing proximity observations never authorize network
startup. The Mac is not used for routine iPhone OTA and must never remember the OTA SSID.

## Partition and rollback behavior

The 4 MB target layout contains equal 1.5 MiB `ota_0` and `ota_1` partitions plus `otadata`.
`esp_ota_get_next_update_partition(NULL)` selects the inactive slot. HTTP cannot select another
address. The application must fit the entire slot including its native signature block.

After selection, the bootloader marks the new image pending verification. Startup first initializes
NVS, capture storage, listen-only CAN, BLE, and the encrypted transport readiness gate. Capture-store
failure explicitly marks the probationary image invalid; CAN/BLE/readiness failures reset before
validation and invoke the bootloader rollback path. Only a successful startup calls
`esp_ota_mark_app_valid_cancel_rollback()`. The next healthy image compares the running and
last-invalid partition to the persisted pending partition and reports `POST_PASSED` or
`ROLLED_BACK`. Wi-Fi remains off during this POST; physical acceptance separately exercises its
one-shot activation and upload path.

## Acceptance matrix

Automated release validation must prove:

- classic ESP32 target and equal A/B slots;
- rollback enabled;
- signed-on-update ECDSA verification and pinned public key;
- private build signing disabled in repository config;
- normal boot has no OTA activation call;
- encrypted BLE command and status characteristics;
- encrypted-link and listen-only activation checks;
- capture pause and failure recovery;
- random WPA2 credentials, hidden SSID, PMF, one station, and five-minute lease;
- constant-time bearer comparison;
- exact-size, streaming SHA-256, native signature, inactive-slot, and boot-selection checks;
- one POST route, no GET/browser surface, and no CAN transmit authority.

Physical release acceptance must additionally prove:

1. normal boot emits no status or OTA SSID;
2. iPhone joins only after the system update prompt is accepted;
3. wrong password, wrong bearer token, wrong length, wrong SHA, unsigned image, and wrong ECDSA key
   are rejected without changing the running slot;
4. successful update reconnects over the preserved BLE identity and reports `POST_PASSED`;
5. forced POST failure rolls back and reports `ROLLED_BACK`;
6. power loss during multiple upload offsets leaves one bootable slot;
7. timeout/cancel/failure stop Wi-Fi and resume capture;
8. iPhone and Mac return to their prior networks and retain no gateway preferred network; and
9. USB backup restore still returns the unit to its exact pre-test image.

Until items 1–9 are recorded with exact firmware commit, artifact hashes, serial trace, and iPhone
build, the feature is a development implementation rather than a field-accepted updater.
