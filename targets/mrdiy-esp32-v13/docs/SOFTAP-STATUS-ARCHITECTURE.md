# Authenticated SoftAP status architecture

Status: development implementation for `v0.1.0-dev.6`

## 1. Problem

BLE is the primary iPhone-to-gateway transport, but BLE alone makes early field diagnosis harder.
When commissioning fails, an engineer needs a second local observation surface that can answer:

- Did the expected firmware boot?
- Is BLE advertising, connected, encrypted, and subscribed?
- Is the TWAI controller running in listen-only mode?
- Are vehicle-bus frames arriving, dropping, or producing controller errors?
- Which OTA slot is running and what rollback state does the bootloader report?
- Is storage or supply telemetry actually available?

The answer must not depend on the iOS BLE state machine that is being diagnosed. The ESP32 already
contains Wi-Fi hardware, so a short-lived local SoftAP and browser page provide an independent
commissioning channel without adding cloud infrastructure.

## 2. Goals

| ID | Requirement | Why |
| --- | --- | --- |
| SAP-001 | Start a WPA2-protected SoftAP for 15 minutes after boot. | Provides a predictable commissioning window while limiting continuous radio exposure. |
| SAP-002 | Generate a unique per-device random credential and persist it in NVS. | Avoids a shared fleet password and survives application-only updates. |
| SAP-003 | Require HTTP Basic authentication on every registered route. | A station that joins the WLAN must still authenticate to the application surface. |
| SAP-004 | Register only read-only `GET` routes. | Makes the absence of mutation structural rather than dependent on UI discipline. |
| SAP-005 | Serve a self-contained page from firmware. | Works without internet, DNS, a package CDN, or a companion service. |
| SAP-006 | Poll a versioned JSON status contract. | Separates presentation from truth and permits automated verification. |
| SAP-007 | Reuse the same real CAN, BLE, runtime, storage, and OTA sources used by firmware. | Prevents the web page from becoming an inconsistent second truth store. |
| SAP-008 | Represent unavailable measurements explicitly. | Prevents unknown supply voltage or storage free space from appearing healthy. |
| SAP-009 | Keep CAN in hardware listen-only mode with a zero-length transmit queue. | The diagnostic surface must not change vehicle-bus risk. |
| SAP-010 | Keep BLE operational while SoftAP is active. | The page is an additional observer, not a replacement transport. |
| SAP-011 | Emit lifecycle events to UART without logging HTTP Authorization headers. | Supports diagnosis without leaking reusable application credentials through request logs. |
| SAP-012 | Stop HTTP and Wi-Fi automatically at the deadline. | Bounds power, coexistence, and attack-surface cost. |

## 3. Non-goals

The milestone intentionally does not provide:

- a captive-portal DNS server;
- connection to the owner's home or phone hotspot network;
- public-internet reachability;
- cloud telemetry forwarding;
- CAN or OBD-II transmit commands;
- PID probing or protocol selection;
- firmware upload, OTA activation, rollback selection, or reboot controls;
- configuration writes;
- log deletion, NVS erase, bond deletion, or credential rotation over HTTP;
- a substitute for the native iPhone application;
- TLS certificate enrollment.

Each omission removes a write authority, external dependency, or security commitment that is not
needed to answer the commissioning questions.

## 4. System context

```text
vehicle bus
    |
    v
TWAI controller -- hardware listen-only --> vhos_can health snapshot
                                             |
                                             +--> BLE gateway.health frames --> iPhone
                                             |
                                             +--> HTTP status JSON ---------> local browser

NimBLE GAP/GATT state -----------------------> BLE status snapshot ---------+
ESP-IDF runtime / reset / heap --------------> runtime status --------------+
partition + OTA APIs ------------------------> OTA status ------------------+
SPIFFS partition inspection ----------------> storage status --------------+
```

The web module owns presentation, authentication, and SoftAP lifecycle. It does not own vehicle
health facts. Source modules expose immutable snapshots that the web handler copies at request
time.

## 5. Component responsibilities

### `vhos_can`

Owns TWAI initialization and controller counters. Its snapshot provides received frames, dropped
frames, bus errors, bus-off transitions, bitrate, controller-running state, and the invariant that
the controller was configured listen-only.

Why: duplicating TWAI calls in the HTTP module would produce races and two interpretations of bus
state.

### `vhos_ble`

Owns NimBLE state and exposes a synchronized snapshot containing advertising, connection,
encryption, notification-subscription, ATT MTU, and negotiated connection parameters.

Why: HTTP runs on another FreeRTOS task. A copied snapshot prevents the page from reading a set of
globals while a GAP event is modifying them.

### `vhos_status_web`

Owns:

- random credential creation and NVS persistence;
- SoftAP name and configuration;
- the 15-minute deadline;
- HTTP authentication;
- HTML and JSON responses;
- runtime, partition, storage, and OTA inspection; and
- station-count tracking.

It owns no CAN or BLE behavior.

### `main`

Constructs the stable gateway identity, starts CAN and BLE, waits for BLE readiness, starts the
SoftAP status service, and records whether the optional observation surface became ready.

Why: startup ordering stays visible in one place. CAN and BLE remain the primary safety-critical
path if the status service cannot start.

## 6. Boot and expiration sequence

1. Initialize the existing NVS partition.
2. Derive the gateway ID and display names from the hardware MAC.
3. Start TWAI in listen-only mode.
4. Start NimBLE and wait for its ready semaphore.
5. Open the `vhos_status` NVS namespace.
6. Load the existing status credential or generate and commit a new random credential.
7. Initialize ESP-NETIF and the default event loop.
8. Create the default SoftAP interface.
9. Configure WPA2-PSK, protected management frames, one station maximum, and RAM-only Wi-Fi
   driver storage.
10. Start the HTTP server and register only authenticated `GET` handlers.
11. Record the monotonic expiration deadline and start a lifecycle task.
12. At 15 minutes, stop HTTP first, then stop Wi-Fi. CAN and BLE continue.

The ordering deliberately brings the primary vehicle observer online before the secondary browser
surface. Expiration stops incoming requests before removing the network interface.

## 7. Identity and addressing

- SoftAP SSID: `VHOS-STATUS-XXXXXX`, where `XXXXXX` is the final three bytes of the Wi-Fi MAC.
- HTTP username: `vhos`.
- HTTP and WPA2 password: a generated, per-device, NVS-persisted random value.
- Default ESP-IDF SoftAP address: `192.168.4.1`.
- Browser entry point: `http://192.168.4.1/`.

The SSID is an identifier, not a secret. The password is never derived from the public MAC or
gateway ID.

## 8. Data lineage

| Displayed conclusion | Source | Transformation |
| --- | --- | --- |
| Firmware identity | `esp_app_get_description()` and `VHOS_BUILD_ID` | String copy only. |
| Uptime | `esp_timer_get_time()` | Microseconds converted to integer milliseconds. |
| Reset reason | `esp_reset_reason()` | Enum mapped to a stable string. |
| BLE state | `vhos_ble_get_health()` | Synchronized copy; no inference beyond explicit booleans. |
| Vehicle-bus activity | `vhos_can_get_health()` | Counters copied; `bus_detected` is true only after at least one received frame. |
| Listen-only enforcement | `vhos_can_get_health()` | Reports the configuration invariant established at driver startup. |
| Storage partition | ESP partition API | Partition presence and capacity are factual; free bytes remain null until mounted accounting exists. |
| Supply | No implemented source | `available=false`, `millivolts=null`. |
| OTA slot | ESP partition and OTA APIs | Running/boot/next labels and image state mapped without initiating an update. |
| AP time remaining | Monotonic deadline | Saturating subtraction; never negative. |

## 9. Resource and coexistence decisions

The page contains no images, fonts, libraries, or remote assets. This reduces flash size, heap use,
DNS requirements, and supply-chain dependencies. The browser polls every two seconds, matching the
existing BLE health cadence and avoiding unnecessary radio activity.

Only one Wi-Fi station is allowed. BLE and Wi-Fi share the ESP32 2.4 GHz radio, so the page is a
commissioning tool rather than an always-on dashboard. The 15-minute deadline bounds coexistence
impact and power use.

## 10. Failure behavior

| Failure | Behavior | Reason |
| --- | --- | --- |
| Credential NVS read/write fails | Status service does not start; CAN and BLE continue. | Never fall back to a public or shared password. |
| Wi-Fi initialization fails | Log the ESP-IDF error; CAN and BLE continue. | The secondary surface must not take down the primary observer. |
| HTTP server fails | Stop Wi-Fi and report the startup failure. | Avoid leaving an authenticated WLAN with no useful service. |
| A status source fails | Return its availability/error state and null values. | Preserve epistemic honesty. |
| Unauthorized request | Return `401` and a Basic challenge, with no status body. | Avoid information disclosure before application authentication. |
| Unsupported method or route | ESP-IDF rejects it; no mutation handler exists. | Default deny. |
| Deadline expires during use | HTTP and SoftAP stop; CAN and BLE remain active. | Time boundary takes precedence over convenience. |

## 11. Deferred decisions

Before production, choose an owner enrollment and recovery design that does not depend on UART.
Candidates include a physical-presence button, QR label, secure BLE provisioning with authenticated
identity, or a manufacturing-injected credential. HTTPS or an app-pinned local protocol must be
evaluated before treating the browser channel as suitable for sensitive data.
