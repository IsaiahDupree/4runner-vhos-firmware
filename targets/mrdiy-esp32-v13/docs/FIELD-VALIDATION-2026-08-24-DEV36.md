# Dev36 atomic retained-history BLE delivery

## Status

Development candidate. No device flash was performed.

## Confirmed failure

The returned trace stayed connected for about 58 seconds. Ending the test requested retained
history; 105 records arrived, then the link disconnected about 5.8 seconds later. Saved recovery
intent repeated reads near offsets 90, 105, and 125. Repetition eventually allowed a physical link
while GATT discovery timed out until an ESP32 power cycle.

Dev35 can encode five records in a 232-byte VHOS frame. At an ATT MTU of 185 that logical frame
needs two notifications. Resource exhaustion after the first one leaves the receiver behind a
partial frame, so the prior implementation terminated the connection to restore framing.

## Dev36 contract

1. A response contains at most three 36-byte records. With both headers, the maximum frame is 160
   bytes.
2. Each response starts only when the negotiated MTU carries that whole frame in one notification.
   A maximum-size response requires `MTU >= 163`; a smaller MTU defers any response it cannot carry
   with zero bytes delivered.
3. `ENOMEM`, `EBUSY`, and `EAGAIN` get at most eight admission attempts with seven 50 ms waits
   (350 ms total wait budget) for a history response. Exhaustion defers that response and keeps the
   application epoch alive, allowing the client to retry the same offset.
4. `BLE_GAP_EVENT_NOTIFY_TX` is recorded according to its synchronous attempt-result behavior. It
   is not treated as a later buffer-credit signal or proof of remote receipt.
5. A non-history partial logical frame retains fresh-epoch recovery because its boundary cannot be
   reconstructed safely in place.
6. BLE waits stay on the dedicated TX task. They never block the priority-12 CAN RX task or the
   independent capture writer. Health remains scheduled every two seconds.

## Evidence counters

Gateway health carries cumulative admission, deferral, pressure, exhaustion, and error counters.
The local status document carries queue depth, high-water mark, epoch, frame totals, attempt totals,
and the last NimBLE result.

Serial outcomes include `BLE_HISTORY_FRAME_NIMBLE_ACCEPTED`, `BLE_HISTORY_TRANSFER_DEFERRED`,
`BLE_NOTIFY_BACKPRESSURE`, `BLE_NOTIFY_BACKPRESSURE_RECOVERED`,
`BLE_NOTIFY_BACKPRESSURE_EXHAUSTED`, and `BLE_NOTIFY_ATTEMPT_RESULT`.

## Device-free verification

`tools/test_mrdiy_ble_transfer_policy.sh` compiles and executes the policy used by firmware. It
proves atomic delivery at MTUs 185 and 247, fail-closed deferral at MTUs 23 and 162, the 350 ms
history-pressure budget, and the exact keep-epoch boundary. It covers the returned incident offsets
and all 3,954 responses needed for the 11,862-observation field corpus. The release validator also
requires the atomic-frame, bounded-deferral, health, and telemetry source contracts.

## Remaining physical validation

1. Build and sign dev36 through the release pipeline.
2. Preserve NVS, bonds, capture storage, OTA metadata, and the inactive slot during installation.
3. Confirm MTU negotiation and saved-bond reconnect without Pair or Forget.
4. Transfer a nonempty capture completely and verify hashes while health remains fresh.
5. Run 20 disconnect/reconnect/transfer cycles and a 30-minute live-CAN soak.
6. Require zero unexplained reset, GATT-service loss, stale epoch leakage, CAN observer drops,
   capture-queue drops, and storage-write failures in combined serial/mobile evidence.
