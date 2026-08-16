# SoftAP status security model

## 1. Security objective

Provide nearby, temporary, read-only commissioning visibility without creating vehicle-control,
internet, fleet-wide credential, or permanent WLAN exposure.

## 2. Protected assets

- BLE bond and identity records in NVS;
- the per-device SoftAP/application password;
- local gateway identity and runtime state;
- vehicle-bus activity and error counters;
- OTA slot and rollback state;
- continued passive-only operation of the CAN controller; and
- availability of the BLE/iPhone health path.

## 3. Trust assumptions

For the development milestone:

- physical USB/UART access is trusted for credential recovery;
- an attacker may be within 2.4 GHz radio range;
- the local browser may be untrusted until it supplies the application credential;
- the vehicle bus is untrusted input;
- the SoftAP has no upstream route to the internet; and
- the status page contains no secret capable of authorizing vehicle control because no such HTTP
  authority exists.

Production hardware should narrow the physical-console assumption and replace it with an explicit
owner-enrollment design.

## 4. Authentication layers

### WPA2-PSK

The WLAN requires a unique per-device random password. Protected management frames are required,
and only one station may associate.

Why both uniqueness and station limit matter: a shared password turns one disclosure into a fleet
incident, while multiple stations increase the chance of an unnoticed observer and radio load.

### HTTP Basic

Every registered route independently requires username `vhos` and the same device password. The
expected `Basic` header is generated in memory and compared without early exit.

Why add application authentication after WPA2: association proves knowledge of the WLAN password,
but route-level authentication prevents accidental disclosure through future network configuration
changes and makes the HTTP boundary explicit and testable.

## 5. Credential creation and persistence

1. Open NVS namespace `vhos_status` read/write.
2. Read key `password`.
3. If a value of the exact expected length exists, reuse it.
4. Otherwise, fill random bytes with the ESP hardware RNG.
5. Map bytes into a restricted unambiguous alphabet.
6. Store and commit the generated value before starting Wi-Fi.

There is no hardcoded fallback. If persistence fails, the SoftAP does not start.

The credential survives application-only flashing because the web flasher protects the NVS range
`0x9000–0xcfff`. It is printed to UART at boot for development recovery and is never returned by an
HTTP or BLE status field.

## 6. Why HTTP is not treated as confidential transport

The page uses HTTP Basic over an isolated WPA2 WLAN, not HTTPS. WPA2 encrypts the radio link, but
Basic credentials are not independently encrypted above that link. Any already-associated station
could observe application traffic if lower-layer isolation fails.

This is accepted only for the time-bounded development surface because:

- the AP has no upstream internet route;
- only one station is permitted;
- the window is 15 minutes;
- the credential is unique per device; and
- HTTP has no mutation or vehicle-control authority.

This is not a claim that HTTP Basic is adequate for a permanent or remotely reachable product.
Production must evaluate a pinned application protocol, HTTPS enrollment, or another mutually
authenticated local channel.

## 7. Attack-surface controls

| Control | Effect |
| --- | --- |
| Boot-bounded 900-second window | Removes the WLAN and server after commissioning time. |
| One station maximum | Limits concurrent observers and radio contention. |
| RAM-only Wi-Fi driver storage | Avoids a second implicit Wi-Fi credential store. |
| Three exact `GET` routes | Prevents wildcard routing from accidentally exposing new handlers. |
| No write methods | Structurally excludes HTTP mutation. |
| No CORS headers | Prevents normal cross-origin browser reads. |
| `Cache-Control: no-store` | Discourages persistence of local status. |
| Strict CSP | Blocks external scripts, frames, objects, and form submission. |
| No cookies or analytics | Removes tracking and session state. |
| No Authorization logging | Avoids credential leakage through UART logs. |
| Explicit nulls | Prevents an unavailable sensor from masquerading as a safe value. |
| Existing listen-only TWAI configuration | Keeps browser activity independent of vehicle-bus output. |

## 8. Default-deny authority matrix

| Operation | HTTP authority |
| --- | --- |
| Read runtime and subsystem status | Allowed after authentication. |
| Read the SoftAP password | Denied; not represented. |
| Transmit raw CAN | No route and no firmware transmit queue. |
| Send OBD-II diagnostics | No route. |
| Change bitrate or pins | No route. |
| Start or stop capture | No route. |
| Upload or activate firmware | No route. |
| Reboot or factory reset | No route. |
| Erase storage, NVS, or BLE bonds | No route. |
| Extend the AP deadline | No route. |

## 9. Residual risks and required follow-up

- UART reveals the development credential to someone with physical access.
- WPA2 and BLE coexistence can affect radio timing and requires endurance validation.
- The browser channel does not have application-layer encryption.
- The boot window appears after every power cycle; production should require explicit physical or
  owner-authorized enablement.
- Credential rotation has no owner UI in this milestone.
- Denial of service by radio interference remains possible.
- Runtime counters can reveal that a vehicle is active, so the page must never be exposed through
  station mode, port forwarding, or a public tunnel.

These risks are documented constraints, not hidden implementation gaps.
