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
        "random identity selection": "ble_hs_id_infer_auto(1, &own_address_type)" in implementation,
        "repeat-pair recovery": "BLE_GAP_REPEAT_PAIRING_RETRY" in implementation,
        "identity epoch evidence": "BLE_IDENTITY_READY type=random-static" in implementation,
    }
    for control, present in required_fragments.items():
        require(present, f"BLE bond-loss recovery is missing required control: {control}")

    require(
        implementation.index("nvs_set_blob(") < implementation.index("nvs_commit(handle)"),
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
    return "passed:persistent-random-static-identity-epoch"


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
    if args.listen_only_source_dir is not None:
        source_files = list(args.listen_only_source_dir.glob("*.c")) + list(
            args.listen_only_source_dir.glob("*.h")
        )
        require(source_files, "listen-only source directory is empty")
        source_text = "\n".join(path.read_text(encoding="utf-8") for path in source_files)
        require("TWAI_MODE_LISTEN_ONLY" in source_text, "listen-only TWAI mode is not enforced")
        require("twai_transmit" not in source_text, "target contains a TWAI transmit path")
        listen_only_check = "passed"

    status_surface_check = "not_applicable"
    if args.status_source_dir is not None:
        status_surface_check = validate_status_surface(args.status_source_dir)
        require(
            "CONFIG_VHOS_STATUS_SOFTAP_AUTOSTART=y" not in sdkconfig,
            "release configuration enables status SoftAP autostart",
        )
        status_surface_check = "passed:compiled_source-default_off_release"

    ble_bond_loss_recovery_check = "not_applicable"
    if args.listen_only_source_dir is not None and (
        args.listen_only_source_dir / "vhos_ble.c"
    ).is_file():
        ble_bond_loss_recovery_check = validate_ble_bond_loss_recovery(
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
            "bleBondLossIdentityRecovery": ble_bond_loss_recovery_check,
            "authenticatedReadOnlyStatusSurface": status_surface_check,
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
