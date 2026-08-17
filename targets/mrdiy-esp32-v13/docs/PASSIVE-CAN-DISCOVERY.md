# Passive CAN discovery

Status: implemented in `v0.1.0-dev.10`

## Purpose

The first accepted vehicle health stream reported zero received frames at a fixed 500 kbit/s. That
result did not distinguish a sleeping vehicle, a 250-kbit CAN network, disconnected CAN wiring, an
unsupported non-CAN OBD transport, or a hardware/transceiver fault. This milestone adds bounded
bitrate observation without adding any vehicle transmit authority.

## State machine

```text
PROBING_500K -- 10 s and <3 valid frames --> PROBING_250K
     |                                        |
     +-- >=3 valid frames --> LOCKED_500K     +-- >=3 valid frames --> LOCKED_250K
                              ^                |
                              +-- 10 s and <3 -+
```

If neither window observes traffic, the firmware continues alternating. Once a window receives at
least three CRC-valid CAN frames, it locks that bitrate until reboot. The threshold rejects a
single accidental observation as sufficient evidence. It is intentionally not an OBD confirmation
threshold.

The receive task also records standard (11-bit) and extended (29-bit) identifiers. The passive
candidate is one of `CAN_11_500`, `CAN_29_500`, `CAN_MIXED_500`, `CAN_11_250`, `CAN_29_250`, or
`CAN_MIXED_250`. The candidate describes observed CAN framing only.

## Safety boundary

- Every controller installation uses `TWAI_MODE_LISTEN_ONLY`.
- The transmit queue length is zero.
- No call to `twai_transmit` exists in the target.
- No OBD request, arbitration ID, diagnostic payload, or active acknowledgement is generated.
- Bitrate transitions are local controller reconfiguration, not vehicle-bus messages.
- SoftAP remains off by default and is not involved in the probe.
- A passive lock never sets `obd_protocol_confirmed`.

These constraints are checked by release validation. The iPhone may display a passive vehicle-bus
candidate, but the OBD-II summary remains unverified until a separate allowlisted diagnostic read
and independent corroboration are recorded.

## Health evidence

The BLE `gateway.health` frame and optional read-only status page expose:

| Field | Meaning |
| --- | --- |
| `can_scan_state` | Current probing, locked, or error state. |
| `can_scan_cycles` | Completed bitrate transitions since boot. |
| `can_bitrate_bps` | Bitrate currently installed in TWAI. |
| `can_passive_lock` | Whether the multi-frame lock threshold was met. |
| `can_standard_frames` / `can_extended_frames` | Valid frame-format counters. |
| `can_frames_500k` / `can_frames_250k` | Valid frames attributed to each bitrate. |
| `passive_can_candidate` | Nullable passive framing candidate. |
| existing drop/error/bus-off counters | Cumulative across controller reinstallations. |

Counters are real controller/task observations. Unknown supply, motion, storage, and OBD protocol
remain null or explicitly unknown; they are not inferred from bus activity.

## Vehicle acceptance procedure

1. Record vehicle/VIN applicability and whether ignition is off, on, or engine running.
2. Connect the gateway and keep the iPhone close enough for a stable encrypted BLE session.
3. Confirm firmware `v0.1.0-dev.10`, listen-only enforcement, and `can_controller_running=true`.
   The handshake must report active configuration `mrdiy-v13-passive-can-scan` version `0.2.0`.
4. Observe at least one full 500/250 cycle with the ignition on or engine running.
5. If a passive lock occurs, record the candidate, standard/extended counts, per-bitrate counts,
   drop/error/bus-off counters, firmware build, and timestamps.
6. Repeat after a reboot. A candidate is useful only when the same bitrate and framing recur.
7. Do not promote OBD-II. A passive lock can be ordinary vehicle CAN traffic rather than a
   diagnostic response.

If both windows remain at zero with zero controller errors, inspect physical DLC pin mapping,
transceiver power/standby, and vehicle wake state. If the physical CAN path is correct, move to the
dedicated all-protocol interpreter workflow for ISO 9141-2, ISO 14230-4, or SAE J1850 candidates;
this classic MrDIY CAN-only target cannot test those electrical transports.
