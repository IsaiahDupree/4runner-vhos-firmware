# Device naming

The OBD/CAN gateway advertises `VHOS-4R-OBD-<MAC suffix>`. For the current development board this
is `VHOS-4R-OBD-B08D14`.

The suffix is derived from the last three station-MAC bytes and is stable across boots and firmware
updates. The BLE name is a discovery and owner-facing label only. The full `esp32-<MAC>`
`gateway_id` in the VHOS handshake remains the immutable evidence source ID. CoreBluetooth UUIDs
and Android BLE addresses are transport metadata and must never replace either identity.

Legacy app versions may observe `VHOS-MRDIY-B08D14`. New clients normalize that legacy label to
the canonical name without deleting bonds or rewriting captured evidence.
