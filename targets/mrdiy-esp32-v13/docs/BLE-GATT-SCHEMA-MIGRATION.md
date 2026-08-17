# BLE GATT schema migration

Status: implemented in `v0.1.0-dev.13`

## Why this exists

iOS caches a bonded peripheral's GATT service and characteristic database. Keeping the same BLE
identity while adding or removing a VHOS characteristic can leave Core Bluetooth using obsolete
attribute handles. The radio link may connect normally while the app remains at `VALIDATING` and
reports the versioned VHOS service as `NOT FOUND`.

Requiring an owner to open iPhone Bluetooth settings and repeatedly select “Forget This Device” is
not an acceptable recovery design.

## Firmware contract

The gateway persists two values in the `vhos_ble_id` NVS namespace:

- `identity_v1`: the random-static BLE identity used across ordinary reboots and OTA updates.
- `gatt_schema`: the integer version of the compiled GATT database.

An ordinary reboot with the same schema keeps the identity and bond. When the compiled schema does
not match the persisted schema, the gateway performs one migration before advertising:

1. Clear obsolete NimBLE security and CCCD records.
2. Generate and persist a new random-static identity.
3. Persist the current GATT schema version.
4. Advertise the complete VHOS service UUID from the new identity.

The identity remains stable again after that one migration. The iPhone sees a new peripheral,
performs service discovery, and creates a fresh encrypted bond without manual Settings cleanup.

## Versioning rule

Increment `VHOS_BLE_GATT_SCHEMA_VERSION` whenever a release changes any registered service,
characteristic, descriptor, UUID, characteristic property, or attribute ordering. Do not increment
it for application payload-only changes that leave the GATT database identical.

The schema version is independent of the firmware release version. Downgrading to a different
schema also causes a one-time rotation because any mismatch is treated as a database change.

## Evidence and field verification

The migration emits:

```text
BLE_GATT_SCHEMA_MIGRATION stored=<old> current=<new> action=rotate-identity-clear-bonds
BLE_IDENTITY_READY type=random-static source=generated gatt_schema=<new> address=<new-address>
```

Subsequent boots must emit `source=persisted` with the same address and schema. Verify that the app
then progresses through service discovery, characteristic discovery, encryption, notification
subscriptions, handshake, and live health without asking the user to forget the prior device.

## Safety boundary

This migration affects only the local BLE identity and bond material. It does not erase the passive
CAN flight recorder, OTA partitions, release verification key, vehicle configuration, or other NVS
namespaces. CAN remains listen-only throughout startup and migration.
