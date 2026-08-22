#!/usr/bin/env python3
"""Validate a VHOS merged image and emit machine-readable release evidence."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import struct
import subprocess
from pathlib import Path


ESP_IMAGE_MAGIC = 0xE9
PARTITION_MAGIC = 0x50AA
PARTITION_MD5_MAGIC = 0xEBEB
def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()


def parse_partitions(path: Path) -> dict[str, dict[str, int | str]]:
    contents = path.read_bytes()
    result: dict[str, dict[str, int | str]] = {}
    for offset in range(0, len(contents), 32):
        entry = contents[offset : offset + 32]
        if len(entry) < 32:
            break
        magic, part_type, subtype, address, size, label, flags = struct.unpack(
            "<HBBII16sI", entry
        )
        if magic == 0xFFFF:
            break
        if magic == PARTITION_MD5_MAGIC:
            require(entry[2:16] == b"\xff" * 14, "partition-table MD5 marker is malformed")
            require(
                entry[16:32] == hashlib.md5(contents[:offset]).digest(),
                "partition-table MD5 digest does not match",
            )
            break
        if magic != PARTITION_MAGIC:
            raise ValueError(f"invalid partition-table magic at byte {offset}: 0x{magic:04x}")
        name = label.split(b"\0", 1)[0].decode("ascii")
        result[name] = {
            "type": part_type,
            "subtype": subtype,
            "offset": address,
            "size": size,
            "flags": flags,
        }
    return result


def parse_flash_files(path: Path) -> dict[int, Path]:
    data = json.loads(path.read_text(encoding="utf-8"))
    files = data.get("flash_files")
    if not isinstance(files, dict):
        raise ValueError("flasher_args.json has no flash_files object")
    root = path.parent
    return {int(address, 0): root / filename for address, filename in files.items()}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_status_surface(source_dir: Path) -> str:
    implementation = (source_dir / "vhos_status_web.c").read_text(encoding="utf-8")
    header = (source_dir / "vhos_status_web.h").read_text(encoding="utf-8")
    page = (source_dir / "status_page.html").read_text(encoding="utf-8")
    activation_policy = (source_dir / "Kconfig.projbuild").read_text(encoding="utf-8")

    required_fragments = {
        "15-minute commissioning window": "VHOS_STATUS_WINDOW_SECONDS 900U" in header,
        "WPA2 SoftAP": "WIFI_AUTH_WPA2_PSK" in implementation,
        "protected management frames": "configuration.ap.pmf_cfg.required = true" in implementation,
        "one-station limit": "configuration.ap.max_connection = 1" in implementation,
        "NVS-backed credential": "VHOS_STATUS_NVS_PASSWORD_KEY" in implementation,
        "HTTP Basic credential encoding": "mbedtls_base64_encode" in implementation,
        "constant-time credential comparison": "constant_time_equal" in implementation,
        "explicit read-only evidence flag": '"read_only_http", true' in implementation,
        "default-off activation symbol": "config VHOS_STATUS_SOFTAP_AUTOSTART" in activation_policy,
        "default-off activation value": "default n" in activation_policy,
    }
    for control, present in required_fragments.items():
        require(present, f"status surface is missing required control: {control}")

    require(
        implementation.count(".method = HTTP_GET") == 3,
        "status surface must register exactly three GET routes",
    )
    forbidden_fragments = (
        "HTTP_POST",
        "HTTP_PUT",
        "HTTP_DELETE",
        "HTTP_PATCH",
        "esp_restart",
        "esp_ota_begin",
        "esp_ota_write",
        "nvs_erase_all",
        "twai_transmit",
    )
    for fragment in forbidden_fragments:
        require(fragment not in implementation, f"status surface contains forbidden authority: {fragment}")

    page_lower = page.lower()
    require("<form" not in page_lower, "status page contains a form")
    require("http://" not in page_lower, "status page contains an external HTTP dependency")
    require("https://" not in page_lower, "status page contains an external HTTPS dependency")
    require("websocket" not in page_lower, "status page contains a WebSocket surface")
    require('fetch("/api/v1/status"' in page, "status page does not poll the versioned local API")
    return "passed"


def validate_ble_bond_loss_recovery(source_dir: Path, sdkconfig: str) -> str:
    implementation_path = source_dir / "vhos_ble.c"
    require(implementation_path.is_file(), "BLE identity recovery implementation is missing")
    implementation = implementation_path.read_text(encoding="utf-8")

    required_fragments = {
        "NimBLE bond persistence": "CONFIG_BT_NIMBLE_NVS_PERSIST=y" in sdkconfig,
        "dedicated identity namespace": 'VHOS_BLE_IDENTITY_NAMESPACE "vhos_ble_id"' in implementation,
        "versioned identity key": 'VHOS_BLE_IDENTITY_KEY "identity_v1"' in implementation,
        "static random identity generation": "ble_hs_id_gen_rnd(0, identity)" in implementation,
        "identity NVS write": "nvs_set_blob(" in implementation,
        "identity NVS commit": "nvs_commit(handle)" in implementation,
        "random identity installation": "ble_hs_id_set_rnd(identity.val)" in implementation,
        "direct random-static identity selection": "ble_hs_id_infer_auto(0, &own_address_type)" in implementation,
        "repeat-pair recovery": "BLE_GAP_REPEAT_PAIRING_RETRY" in implementation,
        "identity epoch evidence": "BLE_IDENTITY_READY type=random-static" in implementation,
    }
    for control, present in required_fragments.items():
        require(present, f"BLE bond-loss recovery is missing required control: {control}")

    persist_identity_start = implementation.index("static esp_err_t persist_identity(")
    persist_identity_end = implementation.index(
        "static esp_err_t persist_gatt_schema(",
        persist_identity_start,
    )
    persist_identity = implementation[persist_identity_start:persist_identity_end]
    require(
        persist_identity.index("nvs_set_blob(") < persist_identity.index("nvs_commit(handle)"),
        "BLE identity must be written before its NVS commit",
    )
    require(
        implementation.index("generate_and_persist_identity(handle, &identity)")
        < implementation.index("ble_hs_id_set_rnd(identity.val)"),
        "generated BLE identity must be persisted before it is installed",
    )
    require(
        "ble_hs_util_ensure_addr(0)" not in implementation,
        "legacy public-address selection bypasses the identity epoch",
    )
    require(
        "ble_hs_id_infer_auto(1, &own_address_type)" not in implementation,
        "privacy-address advertising hides the persisted identity epoch from iOS",
    )
    return "passed:persistent-random-static-identity-epoch"


def validate_ble_history_transfer_health_heartbeat(source_dir: Path) -> str:
    implementation_path = source_dir / "vhos_ble.c"
    require(implementation_path.is_file(), "BLE heartbeat implementation is missing")
    implementation = implementation_path.read_text(encoding="utf-8")
    health_task_start = implementation.index("static void health_task(void *argument)")
    health_task_end = implementation.index("static void advertise(void);", health_task_start)
    health_task = implementation[health_task_start:health_task_end]

    required_fragments = {
        "two-second health interval": "#define VHOS_BLE_HEALTH_INTERVAL_MS 2000U" in implementation,
        "bounded health queue wait": "#define VHOS_BLE_HEALTH_QUEUE_WAIT_MS 500U" in implementation,
        "health-specific queue admission": (
            "channel == VHOS_TRANSPORT_CHANNEL_HEALTH" in implementation
            and "xQueueSend(tx_queue, &item, queue_wait)" in implementation
        ),
        "periodic wait uses named interval": (
            "pdMS_TO_TICKS(VHOS_BLE_HEALTH_INTERVAL_MS)" in health_task
        ),
        "history-transfer state remains observable": (
            "vhos_transport_history_transfer_active()" in health_task
            and "BLE_PERIODIC_HEALTH_CONTINUES" in health_task
        ),
        "health remains scheduled": "vhos_transport_send_health()" in health_task,
        "queue failure is observable": "BLE_PERIODIC_HEALTH_QUEUE_FAILED" in health_task,
    }
    for control, present in required_fragments.items():
        require(present, f"BLE history-transfer heartbeat is missing required control: {control}")

    require(
        "BLE_PERIODIC_HEALTH_SUPPRESSED" not in health_task,
        "history transfer still suppresses the periodic health heartbeat",
    )
    require(
        "suppress_periodic_health" not in health_task,
        "history transfer still has a periodic-health suppression predicate",
    )
    require(
        "continue;" not in health_task,
        "health task has an early-continue path that can skip a scheduled heartbeat",
    )
    return "passed:2s-heartbeat-retained-during-history-transfer"


def validate_vehicle_motion_authority(source_dir: Path) -> str:
    transport_path = source_dir / "vhos_transport.c"
    require(transport_path.is_file(), "vehicle-motion transport source is missing")
    transport = transport_path.read_text(encoding="utf-8")
    require(
        r'\"vehicle_motion\":\"UNKNOWN\"' in transport,
        "gateway health must report vehicle motion as UNKNOWN until a target-validated source exists",
    )
    for unsupported_state in ("PARKED", "MOVING"):
        require(
            rf'\"vehicle_motion\":\"{unsupported_state}\"' not in transport,
            f"firmware fabricates unsupported vehicle-motion authority: {unsupported_state}",
        )
    return "passed:unknown-fail-closed-no-park-inference"


def validate_passive_can_probe(source_dir: Path) -> str:
    implementation_path = source_dir / "vhos_can.c"
    require(implementation_path.is_file(), "passive CAN probe implementation is missing")
    implementation = implementation_path.read_text(encoding="utf-8")
    required_fragments = {
        "500 kbit timing": "TWAI_TIMING_CONFIG_500KBITS()" in implementation,
        "250 kbit timing": "TWAI_TIMING_CONFIG_250KBITS()" in implementation,
        "bounded probe window": "VHOS_CAN_PROBE_WINDOW_MS 10000U" in implementation,
        "multi-frame lock threshold": "VHOS_CAN_LOCK_MINIMUM_FRAMES 3U" in implementation,
        "passive lock evidence": "PASSIVE_CAN_LOCK" in implementation,
        "bitrate switch evidence": "PASSIVE_CAN_PROBE_SWITCH" in implementation,
        "standard frame accounting": "message->extd" in implementation,
        "explicit scan state": "VHOS_CAN_SCAN_PROBING_250K" in implementation,
    }
    for control, present in required_fragments.items():
        require(present, f"passive CAN probe is missing required control: {control}")
    require("TWAI_MODE_LISTEN_ONLY" in implementation, "passive CAN probe is not listen-only")
    require("twai_transmit" not in implementation, "passive CAN probe contains a transmit path")
    return "passed:500k-250k-listen-only-multiframe-lock"


def validate_authenticated_wifi_ota(source_dir: Path, sdkconfig: str) -> str:
    implementation_path = source_dir / "vhos_ota_wifi.c"
    transport_path = source_dir / "vhos_transport.c"
    ble_path = source_dir / "vhos_ble.c"
    require(implementation_path.is_file(), "authenticated Wi-Fi OTA implementation is missing")
    implementation = implementation_path.read_text(encoding="utf-8")
    transport = transport_path.read_text(encoding="utf-8")
    ble = ble_path.read_text(encoding="utf-8")

    required_configuration = {
        "signed application updates": "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y" in sdkconfig,
        "ECDSA signed-app scheme": "CONFIG_SECURE_SIGNED_APPS_ECDSA_SCHEME=y" in sdkconfig,
        "pinned verification key": "CONFIG_SECURE_BOOT_VERIFICATION_KEY=" in sdkconfig,
        "A/B rollback": "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in sdkconfig,
        "no release-time private key": "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES is not set" in sdkconfig,
    }
    for control, present in required_configuration.items():
        require(present, f"authenticated Wi-Fi OTA is missing configuration control: {control}")

    required_implementation = {
        "encrypted BLE activation": "!ble.connected || !ble.encrypted" in implementation,
        "listen-only gate": "!can.controller_running || !can.listen_only" in implementation,
        "capture pause": "vhos_capture_store_set_logging(false)" in implementation,
        "capture recovery": "vhos_capture_store_set_logging(true)" in implementation,
        "five-minute lease": "VHOS_OTA_SESSION_WINDOW_SECONDS 300U" in (source_dir / "vhos_ota_wifi.h").read_text(encoding="utf-8"),
        "random network credentials": "esp_fill_random" in implementation,
        "hidden network": "configuration.ap.ssid_hidden = 1" in implementation,
        "WPA2": "WIFI_AUTH_WPA2_PSK" in implementation,
        "protected management frames": "configuration.ap.pmf_cfg.required = true" in implementation,
        "one-station limit": "configuration.ap.max_connection = 1" in implementation,
        "bearer authorization": 'httpd_req_get_hdr_value_str(request, "Authorization"' in implementation,
        "constant-time token comparison": "constant_time_equal" in implementation,
        "inactive OTA partition": "esp_ota_get_next_update_partition(NULL)" in implementation,
        "streaming partition write": "esp_ota_write(" in implementation,
        "whole-image SHA-256": "mbedtls_sha256_update" in implementation,
        "native signature verification": "esp_ota_end(" in implementation,
        "probationary boot selection": "esp_ota_set_boot_partition(" in implementation,
        "persisted outcome": 'persist_outcome(' in implementation,
        "explicit BLE command": "VHOS_MESSAGE_OTA_CONTROL" in transport,
        "signed capability": 'ota.signed-image' in transport,
        "encrypted command characteristic": "BLE_GATT_CHR_F_WRITE_ENC" in ble,
        "encrypted OTA status characteristic": "BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC" in ble,
    }
    for control, present in required_implementation.items():
        require(present, f"authenticated Wi-Fi OTA is missing implementation control: {control}")

    require(
        "vhos_ota_wifi_activate" not in (source_dir / "main.c").read_text(encoding="utf-8"),
        "normal boot path must not activate the OTA network",
    )
    require("twai_transmit" not in implementation, "OTA implementation contains CAN transmit authority")
    require("HTTP_GET" not in implementation, "OTA service must not expose an observer or browser route")
    require(implementation.count("HTTP_POST") == 1, "OTA service must expose exactly one POST route")
    return "passed:encrypted-ble-lease-wpa2-bearer-sha256-native-signature-ab-rollback"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release", required=True)
    parser.add_argument("--merged", type=Path, required=True)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--partition-table", type=Path, required=True)
    parser.add_argument("--flasher-args", type=Path, required=True)
    parser.add_argument("--artifact-url", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--recovery-report", type=Path, required=True)
    parser.add_argument("--sdkconfig", type=Path, default=Path("sdkconfig"))
    parser.add_argument("--chip-family", default="ESP32-S3")
    parser.add_argument("--hardware-family", default="WiCAN-OBD-PRO")
    parser.add_argument(
        "--hardware-revision",
        default="verify physically; upstream firmware reports 1_53",
    )
    parser.add_argument("--upstream-tag", default="v4.50p")
    parser.add_argument("--source-commit")
    parser.add_argument("--bootloader-offset", type=lambda value: int(value, 0), default=0x0000)
    parser.add_argument("--partition-offset", type=lambda value: int(value, 0), default=0x8000)
    parser.add_argument("--otadata-offset", type=lambda value: int(value, 0), default=0xD000)
    parser.add_argument("--app-offset", type=lambda value: int(value, 0), default=0x10000)
    parser.add_argument(
        "--physical-recovery-status",
        choices=("passed", "not_run_no_hardware_connected"),
        default="not_run_no_hardware_connected",
    )
    parser.add_argument("--listen-only-source-dir", type=Path)
    parser.add_argument("--status-source-dir", type=Path)
    args = parser.parse_args()

    require(
        git("status", "--porcelain", "--untracked-files=normal") == "",
        "release validation requires a clean repository so the firmware commit is exact",
    )

    sdkconfig = args.sdkconfig.read_text(encoding="utf-8")
    expected_idf_target = "esp32s3" if args.chip_family == "ESP32-S3" else "esp32"
    require(
        f'CONFIG_IDF_TARGET="{expected_idf_target}"' in sdkconfig,
        f"target is not {args.chip_family}",
    )
    require(
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in sdkconfig,
        "bootloader rollback is disabled",
    )
    require("CONFIG_APP_ROLLBACK_ENABLE=y" in sdkconfig, "application rollback is disabled")

    partitions = parse_partitions(args.partition_table)
    for name in ("otadata", "ota_0", "ota_1"):
        require(name in partitions, f"required partition {name} is missing")
    require(partitions["otadata"]["size"] >= 0x2000, "OTA metadata partition is too small")
    require(
        partitions["ota_0"]["size"] == partitions["ota_1"]["size"],
        "A/B application slots are not equal",
    )
    require(
        args.app.stat().st_size <= int(partitions["ota_0"]["size"]),
        "application image exceeds an OTA slot",
    )
    require(args.app.read_bytes()[0] == ESP_IMAGE_MAGIC, "application image has invalid magic")

    listen_only_check = "not_applicable"
    passive_can_probe_check = "not_applicable"
    if args.listen_only_source_dir is not None:
        source_files = list(args.listen_only_source_dir.glob("*.c")) + list(
            args.listen_only_source_dir.glob("*.h")
        )
        require(source_files, "listen-only source directory is empty")
        source_text = "\n".join(path.read_text(encoding="utf-8") for path in source_files)
        require("TWAI_MODE_LISTEN_ONLY" in source_text, "listen-only TWAI mode is not enforced")
        require("twai_transmit" not in source_text, "target contains a TWAI transmit path")
        listen_only_check = "passed"
        if (args.listen_only_source_dir / "vhos_can.c").is_file():
            passive_can_probe_check = validate_passive_can_probe(args.listen_only_source_dir)

    status_surface_check = "not_applicable"
    if args.status_source_dir is not None:
        status_surface_check = validate_status_surface(args.status_source_dir)
        require(
            "CONFIG_VHOS_STATUS_SOFTAP_AUTOSTART=y" not in sdkconfig,
            "release configuration enables status SoftAP autostart",
        )
        status_surface_check = "passed:compiled_source-default_off_release"

    ble_bond_loss_recovery_check = "not_applicable"
    ble_history_transfer_health_heartbeat_check = "not_applicable"
    vehicle_motion_authority_check = "not_applicable"
    if args.listen_only_source_dir is not None and (
        args.listen_only_source_dir / "vhos_ble.c"
    ).is_file():
        ble_bond_loss_recovery_check = validate_ble_bond_loss_recovery(
            args.listen_only_source_dir,
            sdkconfig,
        )
        ble_history_transfer_health_heartbeat_check = (
            validate_ble_history_transfer_health_heartbeat(args.listen_only_source_dir)
        )
        vehicle_motion_authority_check = validate_vehicle_motion_authority(
            args.listen_only_source_dir
        )

    authenticated_wifi_ota_check = "not_applicable"
    if args.listen_only_source_dir is not None and (
        args.listen_only_source_dir / "vhos_ota_wifi.c"
    ).is_file():
        authenticated_wifi_ota_check = validate_authenticated_wifi_ota(
            args.listen_only_source_dir,
            sdkconfig,
        )

    flash_files = parse_flash_files(args.flasher_args)
    required_flash_offsets = {
        args.bootloader_offset,
        args.partition_offset,
        args.otadata_offset,
        args.app_offset,
    }
    require(required_flash_offsets.issubset(flash_files), "merged image is missing a required segment")
    merged = args.merged.read_bytes()
    require(
        len(merged) > args.bootloader_offset and merged[args.bootloader_offset] == ESP_IMAGE_MAGIC,
        "merged image has invalid bootloader magic",
    )
    for address, source_path in flash_files.items():
        source = source_path.read_bytes()
        require(
            merged[address : address + len(source)] == source,
            f"merged segment at 0x{address:x} differs from {source_path}",
        )

    firmware_commit = git("rev-parse", "HEAD")
    upstream_commit = args.source_commit or git("rev-list", "-n", "1", args.upstream_tag)
    published_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    artifact_hash = sha256(args.merged)

    manifest = {
        "schemaVersion": "1.0.0",
        "release": args.release,
        "channel": "development",
        "publishedAt": published_at,
        "chipFamily": args.chip_family,
        "hardwareFamily": args.hardware_family,
        "hardwareRevision": args.hardware_revision,
        "upstreamTag": args.upstream_tag,
        "sourceCommit": upstream_commit,
        "firmwareCommit": firmware_commit,
        "espIdfVersion": "5.5.3",
        "artifact": {
            "url": args.artifact_url,
            "address": 0,
            "byteCount": args.merged.stat().st_size,
            "sha256": artifact_hash,
        },
    }
    recovery_report = {
        "schemaVersion": "1.0.0",
        "release": args.release,
        "generatedAt": published_at,
        "firmwareCommit": firmware_commit,
        "checks": {
            "chipTarget": f"passed:{args.chip_family}",
            "mergedSegmentsByteExact": "passed",
            "otaABPartitionTopology": "passed",
            "rollbackConfiguration": "passed",
            "applicationFitsBothSlots": "passed",
            "listenOnlyNoTransmitPath": listen_only_check,
            "passiveCanBitrateProbe": passive_can_probe_check,
            "bleBondLossIdentityRecovery": ble_bond_loss_recovery_check,
            "bleHistoryTransferHealthHeartbeat": ble_history_transfer_health_heartbeat_check,
            "vehicleMotionAuthority": vehicle_motion_authority_check,
            "authenticatedReadOnlyStatusSurface": status_surface_check,
            "authenticatedTemporaryWiFiOTA": authenticated_wifi_ota_check,
            "physicalBackupFlashRollbackRestore": args.physical_recovery_status,
        },
        "partitions": partitions,
        "artifactSha256": artifact_hash,
    }

    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    args.recovery_report.write_text(json.dumps(recovery_report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(recovery_report, indent=2))


if __name__ == "__main__":
    main()
