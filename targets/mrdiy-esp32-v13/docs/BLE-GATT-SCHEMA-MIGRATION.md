# BLE GATT schema migration

Status: mismatch migration implemented in `v0.1.0-dev.13`; audited compatible migration added in
`v0.1.0-dev.24`

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

An ordinary reboot with the same schema keeps the identity and bond. An explicitly allowlisted
schema transition whose attribute database has been independently shown to be identical also keeps
the identity, bond, and CCCDs and only advances the stored schema number.

For an unknown, missing, or incompatible schema, the gateway performs one conservative migration
before advertising:

1. Clear obsolete NimBLE security and CCCD records.
2. Generate and persist a new random-static identity.
3. Persist the current GATT schema version.
4. Advertise the complete VHOS service UUID from the new identity.

The identity remains stable again after an incompatible migration. The iPhone sees a new peripheral,
performs service discovery, and creates a fresh encrypted bond without manual Settings cleanup.

## Audited epoch 2 to epoch 6 compatibility

The `v0.1.0-dev.14` epoch-2 source and dev24 epoch-6 source were compared across the complete
custom service definition, standard-service initialization order, security configuration, and
`sdkconfig.defaults`. Both build against ESP-IDF `v5.5.3`. They are identical in every field that
affects the GATT cache or persisted CCCDs:

| Attribute | Epoch 2 | Epoch 6 |
| --- | --- | --- |
| VHOS primary service | `33613EB3-FFCA-42D1-83FA-A18F12B3F123`, handle 14 | identical |
| Command value | `B3D3279B-0244-4D54-A2AB-A1AB47A5FC0A`, handle 16, write/write-no-response/encrypted-write | identical |
| Stream value/CCCD | `265B90C0-A600-4659-BBBD-5CDA411C49CC`, handles 18/19, read/notify/encrypted | identical |
| Status value/CCCD | `BCB5699A-A9B4-49B8-B69B-D2DFF19B41A9`, handles 21/22, read/notify/encrypted | identical |
| OTA value/CCCD | `18D21F8E-D190-4DB3-923C-27BBFC355874`, handles 24/25, read/notify/encrypted | identical |
| Security | bonding, Secure Connections, no-input/no-output, ENC+ID keys | identical |

NimBLE keys persisted CCCDs by peer identity and characteristic value handle. Because those value
handles and permissions did not move, epoch-2 records remain structurally valid. Dev24 therefore
allowlists only stored epoch `2` for bond-preserving migration to `6`, persists `6`, and emits:

```text
BLE_GATT_SCHEMA_MIGRATION stored=2 current=6 compatibility=verified-identical-db action=preserve-identity-preserve-bonds
BLE_IDENTITY_READY type=random-static source=persisted gatt_schema=6 address=<same-address>
```

No other old epoch is assumed compatible. The multiplexing of framed response types over the
stream characteristic changes application routing, not the registered GATT database.

## Versioning rule

Increment `VHOS_BLE_GATT_SCHEMA_VERSION` whenever a release changes any registered service,
characteristic, descriptor, UUID, characteristic property, or attribute ordering. Do not increment
it for application payload-only changes that leave the GATT database identical.

Never preserve bonds based only on matching UUID names. A compatible migration requires an exact
comparison of service order, characteristic order, value handles, CCCD handles, properties,
permissions, security settings, standard-service registration, SDK version, and configuration.
Every accepted old epoch must remain an explicit allowlist entry with that evidence documented.

The schema version is independent of the firmware release version. Downgrading to a different
schema also causes a one-time rotation because any mismatch is treated as a database change.

## Evidence and field verification

An incompatible migration emits:

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
