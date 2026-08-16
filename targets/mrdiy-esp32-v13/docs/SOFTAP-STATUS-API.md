# SoftAP status HTTP and JSON contract

Contract version: `vhos.status/1.0.0`

## 1. Transport contract

The HTTP service listens only on the ESP32 SoftAP interface during its boot-bounded availability
window. All registered routes require HTTP Basic authentication.

| Method | Path | Response | Purpose |
| --- | --- | --- | --- |
| `GET` | `/` | `text/html; charset=utf-8` | Self-contained human status dashboard. |
| `GET` | `/api/v1/status` | `application/json` | Machine-readable authoritative snapshot. |
| `GET` | `/healthz` | `application/json` | Minimal authenticated service liveness result. |

There are no registered `POST`, `PUT`, `PATCH`, or `DELETE` routes.

Every successful response includes:

- `Cache-Control: no-store` because status is live and may contain local identifiers;
- `X-Content-Type-Options: nosniff`;
- `Referrer-Policy: no-referrer`; and
- for HTML, a Content Security Policy that permits only the page's embedded script and style.

## 2. Authentication behavior

Missing or incorrect credentials return:

```http
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Basic realm="VHOS local status"
Cache-Control: no-store
```

The response does not state whether the username or password was wrong. Authorization header
contents are never logged.

## 3. Status object

`GET /api/v1/status` returns one snapshot. The field set is additive within contract version 1;
clients must ignore unknown fields. Removing a field, changing its meaning, or changing its type
requires a new contract version or path.

```text
root
├── contract
├── contract_version
├── observed_at
├── gateway
├── runtime
├── softap
├── ble
├── vehicle_bus
├── storage
├── power
├── ota
└── safety
```

### Root

| Field | Type | Meaning |
| --- | --- | --- |
| `contract` | string | Always `vhos.status`. |
| `contract_version` | string | Semantic contract version, initially `1.0.0`. |
| `observed_at` | string | Gateway monotonic timestamp formatted `monotonic_us:<integer>`. It is not wall time. |

### `gateway`

| Field | Type | Source |
| --- | --- | --- |
| `gateway_id` | string | Stable ID derived from the Wi-Fi station MAC at boot. |
| `hardware_revision` | string | Compiled target identity `MrDIY-CAN-SHIELD-v1.3+`. |
| `firmware_version` | string | ESP application descriptor. |
| `firmware_build_id` | string | Git-derived compile definition. |
| `esp_idf_version` | string | ESP application descriptor. |

### `runtime`

| Field | Type | Meaning |
| --- | --- | --- |
| `uptime_ms` | integer | Monotonic time since boot. |
| `reset_reason` | string | Stable mapping of the ESP reset-reason enum. |
| `free_heap_bytes` | integer | Current free internal heap reported by ESP-IDF. |
| `minimum_free_heap_bytes` | integer | Lowest free heap observed since boot. |

### `softap`

| Field | Type | Meaning |
| --- | --- | --- |
| `active` | boolean | True while Wi-Fi and the HTTP server are intended to be available. |
| `ssid` | string | Public local network name. |
| `ipv4` | string | Local browser address, initially `192.168.4.1`. |
| `authenticated` | boolean | True because this response passed Basic authentication. |
| `connected_stations` | integer | Current station count maintained from Wi-Fi events. |
| `expires_in_seconds` | integer | Saturating seconds until automatic shutdown. |
| `window_seconds` | integer | Configured boot window, initially 900. |

The credential is never returned.

### `ble`

| Field | Type | Meaning |
| --- | --- | --- |
| `ready` | boolean | NimBLE host completed synchronization. |
| `advertising` | boolean | GAP advertising is active according to firmware state. |
| `connected` | boolean | A BLE connection handle is active. |
| `encrypted` | boolean | The active link reported a successful encryption change. |
| `stream_subscribed` | boolean | Client enabled the VHOS stream notification characteristic. |
| `health_subscribed` | boolean | Client enabled the gateway-health notification characteristic. |
| `att_mtu` | integer or null | Negotiated ATT MTU when connected. |
| `connection_interval_units` | integer or null | Active 1.25 ms BLE interval units. |
| `connection_latency` | integer or null | Active peripheral latency. |
| `supervision_timeout_units` | integer or null | Active 10 ms supervision units. |

Connection-specific values are null when no active connection exists.

### `vehicle_bus`

| Field | Type | Meaning |
| --- | --- | --- |
| `controller_running` | boolean | TWAI status query succeeded for the installed controller. |
| `listen_only` | boolean | Firmware-configured listen-only invariant. |
| `bitrate_bps` | integer | Configured passive bitrate, currently 500000. |
| `bus_detected` | boolean | Deterministic derivation: `received_frames > 0`. |
| `received_frames` | integer | Cumulative frames accepted by the receive task. |
| `dropped_frames` | integer | Controller missed plus overrun counters. |
| `bus_error_count` | integer | ESP-IDF TWAI controller counter. |
| `bus_off_count` | integer | Firmware-counted transitions into bus-off. |
| `obd_protocol_confirmed` | boolean | False until an allowlisted diagnostic transaction proves a protocol. |
| `obd_protocol` | string or null | Null until confirmed. |

Passive traffic can prove that a bus is active. It cannot by itself prove the vehicle's OBD-II
diagnostic protocol, so those conclusions remain separate.

### `storage`

| Field | Type | Meaning |
| --- | --- | --- |
| `partition_present` | boolean | A `storage` data partition was found. |
| `mounted` | boolean | A filesystem has been mounted and accounting is available. |
| `total_bytes` | integer or null | Partition capacity when present. |
| `free_bytes` | integer or null | Null until mounted filesystem accounting exists. |
| `reason` | string or null | Explains why free space is unavailable. |

The service does not format or mount an unknown existing filesystem merely to produce a number.

### `power`

| Field | Type | Meaning |
| --- | --- | --- |
| `supply_available` | boolean | Whether a calibrated supply source is implemented. |
| `supply_millivolts` | integer or null | Null in this milestone because no calibrated source exists. |
| `reason` | string or null | Explains unavailability. |

### `ota`

| Field | Type | Meaning |
| --- | --- | --- |
| `running_partition` | string or null | Label of the executing application partition. |
| `boot_partition` | string or null | Label selected for the next boot. |
| `next_update_partition` | string or null | Inactive slot that ESP-IDF would select for an update. |
| `running_image_state` | string | `new`, `pending_verify`, `valid`, `invalid`, `aborted`, `undefined`, or `unavailable`. |
| `rollback_enabled` | boolean | Compiled bootloader policy. |
| `last_invalid_partition` | string or null | ESP-IDF's last invalid application partition, if any. |
| `upload_supported` | boolean | False in this milestone. |

Reading the next update partition does not write OTA metadata or start an update.

### `safety`

| Field | Type | Meaning |
| --- | --- | --- |
| `read_only_http` | boolean | Always true for this contract. |
| `arbitrary_can_transmit_available` | boolean | Always false. |
| `diagnostic_command_available` | boolean | Always false for this milestone. |
| `configuration_mutation_available` | boolean | Always false. |

These fields make the authority boundary machine-visible; they do not replace source and release
validation that proves the absence of transmit handlers.

## 4. Liveness object

`GET /healthz` confirms only that the authenticated HTTP process is responding:

```json
{"status":"ok","read_only":true}
```

It is not vehicle-health evidence and must not be interpreted as CAN, OBD-II, BLE, supply, storage,
or OTA health.

## 5. Browser behavior

The HTML shell paints every field as `WAIT` until the first successful JSON response. It polls
every two seconds. Network or parsing failures display the failure and retain no invented last-known
healthy conclusion. The page shows the remaining AP time and warns that it will disconnect at
expiration.

The page does not send analytics, use cookies, persist status, or contact any internet origin.
