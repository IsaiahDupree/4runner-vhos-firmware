# SoftAP status operations and verification

## 1. Operator workflow

### Boot and retrieve the development credential

Connect a trusted USB serial console at 115200 baud and reset the gateway. Firmware logs one line
containing the local SSID, fixed username, generated password, address, and expiration window.

Treat the password as a device credential. Do not paste it into tickets, screenshots, commit
messages, test fixtures, or public logs.

### Connect

1. Join the logged `VHOS-STATUS-XXXXXX` Wi-Fi network.
2. Open `http://192.168.4.1/`.
3. Enter username `vhos` and the device password when the browser prompts.
4. Confirm the page identifies the expected gateway and firmware build.
5. Watch the remaining-window indicator; the network intentionally disappears at expiration.

The phone or Mac may report that this Wi-Fi network has no internet. That is expected and is part
of the isolation design.

## 2. Command-line verification

Use environment variables or a hidden prompt rather than placing the credential in shell history.

```bash
read -s VHOS_STATUS_PASSWORD
export VHOS_STATUS_PASSWORD

curl -i http://192.168.4.1/api/v1/status
curl --user "vhos:${VHOS_STATUS_PASSWORD}" http://192.168.4.1/healthz
curl --user "vhos:${VHOS_STATUS_PASSWORD}" http://192.168.4.1/api/v1/status
curl -i --user "vhos:${VHOS_STATUS_PASSWORD}" -X POST http://192.168.4.1/api/v1/status

unset VHOS_STATUS_PASSWORD
```

Expected results:

- unauthenticated status request: `401`;
- authenticated liveness request: `200` and read-only liveness JSON;
- authenticated status request: `200` and contract `vhos.status` version `1.0.0`;
- authenticated `POST`: rejected because no handler exists;
- no `Access-Control-Allow-Origin` response header;
- no password field anywhere in the JSON;
- no request Authorization value in UART logs.

## 3. Physical acceptance matrix

| Test | Required evidence |
| --- | --- |
| Fresh NVS credential | A non-default credential is generated, committed, and the service starts. |
| Persisted credential | Application-only reflash preserves the same NVS credential and BLE bond. |
| Unauthorized request | Every registered route returns `401` without a status body. |
| Authorized browser shell | Root route returns self-contained HTML with strict security headers. |
| Authorized status JSON | Required objects and contract version are present. |
| Default deny | POST/PUT/PATCH/DELETE and unknown routes cannot mutate state. |
| CAN safety | TWAI remains listen-only with transmit queue length zero. |
| Data honesty | Supply remains unavailable/null; storage free bytes remain unavailable/null until implemented. |
| BLE coexistence | iPhone reaches verified VHOS handshake while SoftAP and polling are active. |
| Counter lineage | CAN counters on HTTP match the same firmware snapshot source used by BLE health. |
| OTA observation | Running, boot, next, and rollback fields match ESP-IDF partition APIs. |
| Expiration | HTTP stops, the SSID disappears, and BLE/CAN continue after 900 seconds. |
| Reboot window | A new boot creates a new 900-second window without changing the persisted password. |

## 4. Status interpretation

### BLE connected but not encrypted

The radio link exists, but the saved bond or security negotiation has not completed. Do not treat
service discovery or notifications as authoritative until encryption succeeds.

### BLE encrypted but health not subscribed

The client has not enabled the gateway-health characteristic. The web page may still display
gateway-local facts because it reads firmware snapshots independently.

### Zero received frames

This means no frame has reached the receive task since boot. It does not prove that the OBD port is
dead. Possible causes include vehicle power state, wiring, transceiver state, bitrate mismatch, or
an inactive bus. The page must label OBD protocol unconfirmed.

### Storage partition present but free bytes unavailable

The partition table proves capacity, not filesystem accounting. Firmware deliberately does not
format or mount unknown contents only to show a free-space number.

### Supply unavailable

No calibrated voltage-sensing source is implemented in this target. A null value is correct; zero
millivolts would be a false measurement.

## 5. Troubleshooting

### SSID does not appear

Check UART for `VHOS_SOFTAP_READY` or `VHOS_SOFTAP_START_FAILED`. Confirm the boot is within the
15-minute window. A credential storage, ESP-NETIF, Wi-Fi, or HTTP startup failure must leave BLE and
CAN running and report the original ESP-IDF error.

### Browser continually asks for credentials

Confirm the fixed username is lowercase `vhos` and use the password from the current device's UART
line. The SSID suffix is not the password. Do not erase the whole NVS partition as a first recovery
step because it also contains BLE bonds.

### Browser connects but status polling fails

Open `/healthz` with the same credentials. Inspect UART for station join/leave and HTTP allocation
errors. Confirm the address is `192.168.4.1` and the AP has not expired.

### BLE becomes unstable while Wi-Fi is active

Capture both iPhone commissioning traces and UART GAP logs. Record connection interval, latency,
supervision timeout, ATT MTU, Wi-Fi station count, polling cadence, and exact disconnect reason.
Do not weaken BLE security, listen-only CAN, or status authentication as a workaround.

## 6. Release evidence

For every release containing this service, retain:

- firmware commit and build ID;
- merged and OTA application SHA-256 hashes;
- target, partition, rollback, image-size, and no-transmit validation results;
- the automated status-surface authority check: exactly three `GET` routes, WPA2, PMF,
  one station, NVS credential persistence, Basic authentication, constant-time comparison,
  no mutation verbs or restart/erase/OTA-write/CAN-transmit calls, and no external page assets;
- authenticated and unauthenticated HTTP results;
- a redacted status JSON capture;
- BLE handshake and at least a short coexistence run;
- expiration evidence or an explicitly recorded not-yet-run endurance gate; and
- confirmation that NVS was preserved during application-only flashing.

Credentials are never release artifacts.
