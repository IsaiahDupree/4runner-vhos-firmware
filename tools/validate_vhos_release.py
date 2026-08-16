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
REQUIRED_FLASH_OFFSETS = {0x0000, 0x8000, 0xD000, 0x10000}


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
    args = parser.parse_args()

    require(
        git("status", "--porcelain", "--untracked-files=normal") == "",
        "release validation requires a clean repository so the firmware commit is exact",
    )

    sdkconfig = Path("sdkconfig").read_text(encoding="utf-8")
    require("CONFIG_IDF_TARGET=\"esp32s3\"" in sdkconfig, "target is not ESP32-S3")
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

    flash_files = parse_flash_files(args.flasher_args)
    require(REQUIRED_FLASH_OFFSETS.issubset(flash_files), "merged image is missing a required segment")
    merged = args.merged.read_bytes()
    require(merged and merged[0] == ESP_IMAGE_MAGIC, "merged image has invalid bootloader magic")
    for address, source_path in flash_files.items():
        source = source_path.read_bytes()
        require(
            merged[address : address + len(source)] == source,
            f"merged segment at 0x{address:x} differs from {source_path}",
        )

    firmware_commit = git("rev-parse", "HEAD")
    upstream_commit = git("rev-list", "-n", "1", "v4.50p")
    published_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    artifact_hash = sha256(args.merged)

    manifest = {
        "schemaVersion": "1.0.0",
        "release": args.release,
        "channel": "development",
        "publishedAt": published_at,
        "chipFamily": "ESP32-S3",
        "hardwareFamily": "WiCAN-OBD-PRO",
        "hardwareRevision": "verify physically; upstream firmware reports 1_53",
        "upstreamTag": "v4.50p",
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
            "esp32s3Target": "passed",
            "mergedSegmentsByteExact": "passed",
            "otaABPartitionTopology": "passed",
            "rollbackConfiguration": "passed",
            "applicationFitsBothSlots": "passed",
            "physicalBackupFlashRollbackRestore": "not_run_no_hardware_connected",
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
