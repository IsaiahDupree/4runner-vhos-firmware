# BLE bond-loss recovery

Status: implemented in `v0.1.0-dev.9`

## Problem

BLE bonding creates durable state on both peers. The ESP32 stores its security records in NVS,
while iOS stores the matching peripheral relationship in the system keychain. An application-only
firmware update preserves NVS and therefore preserves the relationship. A full-chip erase or a
flash operation that replaces NVS can leave the two peers asymmetric:

- the iPhone remembers a bond and attempts to restore encryption;
- the ESP32 has no matching security record;
- the physical link and ATT MTU can succeed before encryption fails;
- service discovery never becomes authoritative, and automatic reconnect repeats the same
  failure.

The field evidence that led to this design was an ESP32 boot with
`BLE_BOND_STORE our_security_records=0 peer_security_records=0`, followed by an iPhone connection,
ATT MTU 247, and `BLE_ENCRYPTION status=1288`. The status is a peer Security Manager error, not an
OBD-II, CAN, signal-strength, or GATT UUID failure.

Requiring the owner to open iOS Settings and use **Forget This Device** repairs the asymmetry, but
it is not an acceptable normal recovery workflow.

## Identity-epoch rule

The gateway has two coupled pieces of durable BLE state:

1. NimBLE security records, persisted by `CONFIG_BT_NIMBLE_NVS_PERSIST`;
2. a six-byte random-static identity stored as `identity_v1` in the `vhos_ble_id` NVS namespace.

On every NimBLE synchronization:

1. Read `identity_v1` from NVS.
2. If it is valid, install it and advertise with that same identity.
3. If it is absent or malformed, generate a Bluetooth-compliant random-static identity.
4. Commit the generated identity to NVS before installing or advertising it.
5. Advertise the random-static identity directly (`BLE_OWN_ADDR_RANDOM`). Do not select a
   resolvable-private address, because that would hide the identity epoch from Core Bluetooth.

The identity and bond normally survive together. If NVS is erased, they are lost together. The
next boot creates a new identity epoch, so Core Bluetooth assigns a new peripheral identity and
performs a fresh pairing rather than restoring the unusable old relationship.

## Lifecycle behavior

| Event | Identity | Bond | Expected iPhone behavior |
| --- | --- | --- | --- |
| Power cycle or watchdog restart | Preserved | Preserved | Restore and reconnect. |
| Application-only USB flash at `0x10000` | Preserved | Preserved | Restore and reconnect. |
| Signed application OTA | Preserved | Preserved | Restore and reconnect after reboot. |
| Full NVS erase or full-chip erase | Rotates once | Lost | Discover as a new peripheral and pair without forgetting the old entry. |
| Corrupt identity blob | Rotates once after committing replacement | Bond may remain | Treat as a new peripheral; repeat-pair handler may discard an obsolete peer record. |
| NVS open, write, or commit failure | Not advertised | Unchanged/unknown | BLE remains unavailable with an explicit UART error; no unstable identity is exposed. |

The last behavior is deliberate. Generating an address in RAM and advertising it without durable
storage would produce a different peripheral after every reboot, accumulating stale iOS records
and defeating reconnect reliability.

## Security and authority boundaries

- The random-static address is a transport identity, not authentication by itself.
- Characteristics containing commands or evidence still require BLE encryption.
- Secure Connections and bonding remain enabled; no pairing or GATT permission is weakened.
- The recovery mechanism never erases iOS state and does not expose a remote NVS-erase command.
- The status SoftAP remains off by default and is unrelated to the identity decision.
- Passive CAN remains forced to listen-only mode; identity recovery introduces no transmit path.

## UART evidence

A newly erased gateway must report:

```text
BLE_IDENTITY_READY type=random-static source=generated address=<six-byte address>
BLE_BOND_STORE our_security_records=0 peer_security_records=0
```

After successful pairing and a reboot, the gateway must report:

```text
BLE_IDENTITY_READY type=random-static source=persisted address=<same address>
BLE_BOND_STORE our_security_records=1 peer_security_records=1
BLE_ENCRYPTION status=0
```

The exact bond-record counts can grow in a multi-peer development environment, but this target
permits only one concurrent connection and production commissioning should retain only intended
owner records.

## Acceptance procedure

1. Back up the full flash before manipulating NVS.
2. Flash the application partition only and capture the boot log.
3. Confirm identity source `generated` on the first `v0.1.0-dev.9` boot and record its address.
4. In the iOS app, scan and verify the VHOS service, encrypted link, three notification channels,
   versioned handshake, and live health arrival.
5. Reboot without erasing NVS.
6. Confirm identity source `persisted`, the identical address, nonzero bond records, and automatic
   reconnection without **Forget This Device**.
7. In a controlled recovery test, back up NVS, erase only NVS, and reboot.
8. Confirm a new generated address and successful fresh commissioning without removing the prior
   iOS peripheral manually.

Do not claim OBD-II protocol confirmation from this test. BLE transport acceptance and vehicle-bus
evidence are separate gates.
