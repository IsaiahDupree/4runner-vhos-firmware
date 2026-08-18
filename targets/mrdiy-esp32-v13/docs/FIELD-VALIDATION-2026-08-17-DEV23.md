# MrDIY ESP32 deterministic bonded reconnect — v0.1.0-dev.23

Date: 2026-08-17
Status: implementation, reproducible build, USB development flash, restored data path, and two saved-identity iPhone reconnects passed; release signing and durability matrix remain open

## Purpose

This record closes two firmware state-ordering defects exposed by real iPhone reconnect traces.
The defects were not radio discovery, GATT enumeration, CAN, OBD-II, Wi-Fi, or ESP32 reboot
failures. They occurred after NimBLE had already restored valid bond state.

The required invariant is:

> A bonded encrypted reconnect reuses the existing encryption and persisted stream CCCD exactly
> once. GAP `CONNECT` may observe restored state, but it may not erase or restart it.

Fresh unencrypted connections still initiate pairing. Ordinary reconnects must not display a new
pairing prompt and must not require **Forget This Device**.

## Fixed hardware and identity scope

| Field | Value |
| --- | --- |
| Hardware | classic ESP32-D0WDQ6 revision 1.1 + MrDIY CAN Shield v1.3+ |
| Silicon MAC | `94:54:c5:b0:8d:14` |
| USB serial mapping | `/dev/cu.usbserial-0001` during the field runs |
| Immutable gateway ID | `esp32-9454c5b08d14` |
| Owner-facing name | `VHOS-4R-OBD-B08D14` |
| GATT/bond compatibility epoch | `6` |

The application-only update does not intentionally rotate the persisted random-static identity or
erase an existing compatible bond.

## Physical evidence that isolated the defects

### Dev21: redundant security on an already restored link

On a bonded reconnect, NimBLE emitted successful pairing and encryption events before its normal
GAP connect callback. The connect descriptor already reported `encrypted=1` and `bonded=1`.
Dev21 nevertheless ran `ble_gap_security_initiate()` after 150 ms. That redundant procedure later
failed with host status `7` or the fixed 30-second Security Manager timeout `13`, followed by an
iPhone-visible disconnect such as NimBLE/HCI reason `531`.

Dev22 corrected that defect. Its security callout inspects `ble_gap_conn_desc` and emits:

```text
BLE_SECURITY_INITIATE skipped=already-encrypted handle=<n> bonded=1 authenticated=<n> key_size=16
```

Only a descriptor with `encrypted=0` can enter proactive security initiation.

### Dev22: restored CCCD erased by GAP CONNECT

The next synchronized trace exposed a second legal ordering:

```text
BLE_PAIRING_COMPLETE ... status=0
BLE_ENCRYPTION ... status=0 ... encrypted=1 bonded=1
BLE_SUBSCRIBE ... handle=18 reason=3 reason_name=bond-restore notify=0->1
IPHONE_LINK_CONNECTED ...
```

The stream subscription was valid before `CONNECT`. Dev22's connect handler then unconditionally
set every notification flag to false. The iPhone correctly considered the stream subscribed and
sent the handshake; firmware accepted the command but suppressed outbound notifications because
its local stream flag had been erased. The later disconnect was therefore a transport-state bug,
not evidence that the saved bond was invalid.

## Dev23 behavior

Dev23 treats the pre-connect events as authoritative:

1. Boot, host reset, and `DISCONNECT` clear in-memory encryption and subscription state.
2. NimBLE may restore pairing, encryption, identity, and CCCD state before `CONNECT`.
3. A successful `CONNECT` preserves the existing stream, legacy status, and legacy OTA flags.
4. The live connection descriptor supplies the effective encryption state when available.
5. If descriptor lookup is momentarily unavailable, a prior successful `ENC_CHANGE` result is
   retained rather than overwritten.
6. The 150-ms security callout skips an already encrypted link.
7. A genuinely fresh unencrypted link still calls `ble_gap_security_initiate()`.
8. Application traffic remains gated: live health is not emitted until encryption, the stream
   subscription, and an accepted versioned command all agree.

The connect proof line is:

```text
BLE_CONNECT_EFFECTIVE_STATE handle=<n> descriptor_rc=0 encrypted=1 stream_notify=1 \
status_notify=<n> ota_notify=<n> source=preserved-pre-connect
```

Evidence, health, and OTA remain separate framed contracts, but they share the single encrypted
stream characteristic. The status and OTA characteristics remain registered for GATT compatibility;
they are not additional required commissioning subscriptions.

## Physical iPhone evidence

The unsigned development image below was flashed over USB at application offset `0x10000` while
preserving NVS and the existing bond. The attached iPhone and UART traces then established:

1. A state-restored iPhone session adopted the saved peripheral and continuously decoded health
   from `2026-08-17T23:20:08.425Z` through beyond `23:24:26Z`, with no Pair sheet, security timeout,
   disconnect, panic, or reboot.
2. On a later app build, the restored physical stream still delivered health but its cached command
   path did not acknowledge three bounded handshake writes. The app intentionally terminated only
   that link and exposed Reconnect; it did not erase the bond or gateway UUID.
3. At `23:29:51.961Z`, the operator selected Reconnect. Core Bluetooth retrieved the saved UUID,
   connected in approximately 0.5 seconds, subscribed once, and logged
   `HANDSHAKE_VERIFIED firmware=0.1.0-dev.23` at `23:29:53.138Z` on attempt 1.
4. At `23:31:18.424Z`, explicit Disconnect terminated the verified session with automatic reconnect
   disabled. The following explicit Reconnect again targeted the saved UUID, logged the handshake at
   `23:31:20.344Z` on attempt 1, and resumed health with no Pair sheet.
5. The ESP32 trace for the reconnect reported encrypted/bonded state with key size 16, one peer and
   one local bond record, one stream subscription, application-command acceptance, and continued
   notifications. CAN remained listen-only and SoftAP remained disabled.

These runs close the immediate restored-data and manual saved-identity reconnect defects. They do
not replace the repeated cold-launch, gateway-power-cycle, out-of-range, background/foreground, or
ten-minute endurance gates below.

## Exact dev23 build artifact

| Field | Value |
| --- | --- |
| Application version | `0.1.0-dev.23` |
| Local artifact | `targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin` |
| Artifact state | unsigned ESP-IDF application image; flashed over USB for development acceptance; signed release pending |
| Size | 1,048,496 bytes |
| SHA-256 | `664c502a513ca50f0c2b9cc984756ebc0e8e8eb6795ed1b323cdb1660bcdb08b` |
| Smallest application partition | 1,572,864 bytes (`0x180000`) |
| Build free space | 524,368 bytes (`0x80050`) |
| Build environment | `espressif/idf:v5.5.3`, target `esp32` |

This checksum identifies the unsigned local development candidate and the USB-flashed application
used for the physical evidence above. A signed release has a different size and checksum and must
be recorded separately before distribution or OTA. Build success alone remains insufficient; the
physical-pass claims above come from the attached-device traces.

## Safety invariants

Dev23 changes BLE session bookkeeping only. It does not broaden authority:

- TWAI remains forced to `TWAI_MODE_LISTEN_ONLY`.
- No arbitrary CAN transmission, Active Test, or write-capable OBD-II path is compiled.
- SoftAP, station Wi-Fi, and HTTP remain disabled on normal boot.
- Secure Connections, bonding, and encrypted GATT permissions remain enabled.
- An unexpected interactive MITM/passkey action is rejected because this device has no trusted
  display or input path; the intended method remains Secure Connections Just Works.
- Repeat pairing deletes only the exact conflicting peer record before retrying; there is no broad
  NVS erase.
- A compatible application-only flash preserves NVS, capture storage, OTA metadata, BLE identity,
  and bonds.
- GATT schema epoch `6` is unchanged because dev23 changes no service, characteristic, descriptor,
  UUID, permission, or attribute ordering.

## Mac build and diagnostic commands

Run these from the repository root.

Build the target without modifying flash:

```bash
docker run --rm \
  -v "$PWD:/project" \
  -w /project/targets/mrdiy-esp32-v13 \
  espressif/idf:v5.5.3 \
  bash -lc 'idf.py build'
```

Verify the candidate identity:

```bash
shasum -a 256 targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin
stat -f '%z bytes' targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin
strings targets/mrdiy-esp32-v13/build/vhos_mrdiy_esp32_v13.bin \
  | rg '0.1.0-dev.23|BLE_CONNECT_EFFECTIVE_STATE|skipped=already-encrypted'
```

Capture the physical UART without changing firmware. On this Mac, `screen` set the USB-UART baud
reliably without the DTR/RTS resets observed from some Python serial defaults:

```bash
TERM=xterm-256color screen /dev/cu.usbserial-0001 115200
```

For an archived run, use a serial logger that explicitly suppresses DTR and RTS before opening the
port; verify that its first event is not a reset before accepting the evidence.

Filter the security timeline:

```bash
rg 'BLE_(PAIRING_COMPLETE|ENCRYPTION|SUBSCRIBE|CONNECT_EFFECTIVE_STATE|SECURITY_INITIATE|SECURITY_STATE|BOND_STORE)|IPHONE_LINK' \
  /tmp/vhos-dev23-esp32.log
```

Run the Mac Core Bluetooth probe only when the iPhone client is disconnected or out of range;
the target permits one simultaneous BLE connection. The probe validates the same one-CCCD frame
path but does not replace iPhone acceptance:

```bash
tools/run_vhos_ble_probe.sh 60 | tee /tmp/vhos-dev23-mac-probe.log
```

The temporary Python path above is the established local development environment. If it does not
exist after a restart, create a private virtual environment with `esptool` and `pyserial`; do not
install serial tooling into the firmware image or commit that environment.

## Physical acceptance gates

### Fresh pairing

With no compatible bond, the trace must show:

1. effective connect state `encrypted=0 stream_notify=0`;
2. `BLE_SECURITY_INITIATE ... rc=0`;
3. one owner pairing approval completed within NimBLE's 30-second procedure window;
4. `BLE_PAIRING_COMPLETE ... status=0` and `BLE_ENCRYPTION ... status=0`;
5. stream CCCD `reason=cccd-write ... notify=0->1`;
6. accepted handshake, CRC-valid response, and live health frames.

### Bonded reconnect

After a normal app relaunch, gateway reboot, and radio-loss recovery, the trace must show:

1. `BLE_ENCRYPTION ... status=0` with `bonded=1`;
2. stream CCCD `reason=bond-restore ... notify=0->1`, even if it precedes `CONNECT`;
3. `BLE_CONNECT_EFFECTIVE_STATE ... encrypted=1 stream_notify=1`;
4. `BLE_SECURITY_INITIATE skipped=already-encrypted`;
5. handshake and health arrival without another pairing prompt;
6. no later encryption status `7` or `13` from a redundant procedure;
7. no reason `520` supervision timeout or reason `531` user-termination cycle.

Repeat this for at least three consecutive cold app launches and one gateway power cycle. A Mac
probe success is useful development evidence, but only the real iPhone restoration run closes the
owner workflow.

## Remaining claims intentionally withheld

Dev23 is physically accepted for the restored encrypted data path and explicit saved-identity
Disconnect/Reconnect sequence. It is not yet release accepted until a signed image passes the full
matrix above. This work also does not establish OBD-II protocol confirmation, vehicle motion,
supply voltage, active diagnostics, OTA power-loss safety, or CAN signal decoding. Those are
separate evidence gates.
