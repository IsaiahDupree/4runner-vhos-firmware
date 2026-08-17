# SoftAP activation policy and Mac Wi-Fi isolation

Decision status: accepted for `v0.1.0-dev.7`

## 1. Incident and immediate conclusion

The first `v0.1.0-dev.6` bench image automatically started `VHOS-STATUS-B08D14` after every
boot. During physical verification, the Mac was explicitly instructed to join that network. A
Wi-Fi client can be associated with only one infrastructure network on the interface at a time,
so the Mac left its normal access point and received `192.168.4.2` from the ESP32. Internet and
LAN access through the normal network therefore disappeared until the Mac re-associated.

That behavior can look like the surrounding Wi-Fi was disabled, but the ESP32 did not and cannot
change the home router. Two additional risks still justify removing automatic startup:

- macOS may remember and automatically rejoin a development SoftAP that has no internet; and
- BLE plus a SoftAP increase 2.4 GHz radio activity and device power demand even when no page is
  being used.

The saved `VHOS-STATUS-B08D14` preferred network was removed from the development Mac, its Wi-Fi
radio was cycled, and the normal `192.168.1.x` route was restored.

## 2. Decision

The status SoftAP is **off by default**. The normal firmware boot path starts passive CAN and BLE,
logs `VHOS_SOFTAP_DISABLED`, and does not initialize ESP-NETIF, Wi-Fi, DHCP, or HTTP.

The source remains compiled so its security and size are continuously validated. A developer may
enable `CONFIG_VHOS_STATUS_SOFTAP_AUTOSTART` only in a deliberate bench build. That Kconfig option
defaults to `n` and is not enabled in the published target defaults.

The production commissioning action will be an explicit command from an already bonded and
encrypted iPhone session. The user must tap an action, the gateway must confirm the command
contract, and the resulting AP remains limited to one authenticated station and 900 seconds.

## 3. Why Mac-presence detection is rejected

“Enable the AP when the Mac is out of range” sounds convenient but has unsafe failure semantics.
The gateway cannot reliably establish that a particular Mac is present without a cooperative,
authenticated beacon. Modern Apple devices randomize Wi-Fi addresses and do not continuously emit
a stable identity. Passive Wi-Fi probe observation is incomplete, privacy-invasive, and easy to
spoof or jam. BLE discovery would add another shared-radio scan and would still confuse “not seen”
with “not present.”

A proximity rule would therefore fail open: a temporary missed advertisement, sleeping Mac,
changed private address, reboot, or radio interference would unexpectedly turn Wi-Fi on. Absence
of evidence is not evidence of absence, so it is not an acceptable authority signal.

## 4. Activation state machine

```text
BOOT
  |
  +--> passive CAN + BLE ready
  |
  +--> SoftAP OFF  <------------------------------------------+
          |                                                   |
          | explicit bonded/encrypted user request            |
          v                                                   |
      STARTING -- any credential/Wi-Fi/HTTP error --> OFF     |
          |                                                   |
          v                                                   |
       ACTIVE -- 900 s deadline or power cycle ---------------+
```

The future BLE request may turn the read-only observer on; that observer may not extend the
deadline, expose credentials, weaken CAN listen-only mode, or authorize diagnostics, OTA writes,
reboot, erase, or configuration changes.

Firmware `v0.1.0-dev.12` adds a **separate** temporary OTA service with its own encrypted-BLE
activation contract, random per-session credentials, five-minute lease, single POST route, signed
image verification, and A/B rollback. It does not add write authority to the status observer. See
[authenticated temporary Wi-Fi OTA](AUTHENTICATED-WIFI-OTA.md).

## 5. Mac test hygiene

Automated development must never leave a no-internet gateway AP as a preferred Mac network.
Before a bench test, record the current route. Join the VHOS AP only for the bounded HTTP check.
Afterward, remove the VHOS network from the preferred list, cycle Wi-Fi if required, and prove that
the original gateway and address have returned.

The test is incomplete unless all of these are true:

- the normal network is reachable before the test;
- the ESP32 assigns the expected isolated `192.168.4.x` address during the test;
- the HTTP checks finish within the commissioning window;
- the VHOS SSID is not retained for automatic joining; and
- the normal network route is restored after the test.

## 6. Release gates

- `CONFIG_VHOS_STATUS_SOFTAP_AUTOSTART` is absent or disabled in the release sdkconfig.
- A normal boot logs the explicit-disabled state and emits no VHOS SSID.
- BLE reconnect and passive CAN remain available with Wi-Fi uninitialized.
- An explicit activation implementation is not advertised as a capability until the iOS command,
  firmware authorization, response evidence, timeout, and physical coexistence tests all pass.
- No release uses passive Mac detection or missing proximity evidence to turn the AP on.
