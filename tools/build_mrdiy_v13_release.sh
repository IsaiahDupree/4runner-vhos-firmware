#!/usr/bin/env bash
set -euo pipefail

release="${1:-v0.1.0-dev.36}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target_dir="${repo_root}/targets/mrdiy-esp32-v13"
artifact="vhos-mrdiy-esp32-v13-${release}-merged.bin"
ota_artifact="vhos-mrdiy-esp32-v13-${release}-ota.bin"
artifact_url="${VHOS_ARTIFACT_URL:-https://github.com/IsaiahDupree/4runner-vhos-firmware/releases/download/${release}/${artifact}}"
source_commit="${MRDIY_SOURCE_COMMIT:-$(git -C "${repo_root}" rev-parse HEAD)}"
physical_recovery_status="${VHOS_PHYSICAL_RECOVERY_STATUS:-not_run_no_hardware_connected}"
signing_key="${VHOS_SECURE_BOOT_SIGNING_KEY:-}"

cd "${repo_root}"
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "release build requires a clean tracked worktree" >&2
  exit 1
fi

idf.py -C "${target_dir}" reconfigure
idf.py -C "${target_dir}" build
if [[ -z "${signing_key}" || ! -f "${signing_key}" ]]; then
  echo "VHOS_SECURE_BOOT_SIGNING_KEY must identify the external ESP-IDF ECDSA release key" >&2
  exit 1
fi
unsigned_app="${target_dir}/build/vhos_mrdiy_esp32_v13-unsigned.bin"
signed_app="${target_dir}/build/vhos_mrdiy_esp32_v13-signed.bin"
install -m 0644 "${target_dir}/build/vhos_mrdiy_esp32_v13.bin" "${unsigned_app}"
espsecure.py sign_data \
  --version 1 \
  --keyfile "${signing_key}" \
  --output "${signed_app}" \
  "${unsigned_app}"
espsecure.py verify_signature \
  --version 1 \
  --keyfile "${signing_key}" \
  "${signed_app}"
install -m 0644 "${signed_app}" "${target_dir}/build/vhos_mrdiy_esp32_v13.bin"
mkdir -p "${repo_root}/dist"
idf.py -C "${target_dir}" merge-bin --format raw -o "${repo_root}/dist/${artifact}"
install -m 0644 \
  "${target_dir}/build/vhos_mrdiy_esp32_v13.bin" \
  "${repo_root}/dist/${ota_artifact}"

python3 "${repo_root}/tools/validate_vhos_release.py" \
  --release "${release}" \
  --merged "${repo_root}/dist/${artifact}" \
  --app "${target_dir}/build/vhos_mrdiy_esp32_v13.bin" \
  --partition-table "${target_dir}/build/partition_table/partition-table.bin" \
  --flasher-args "${target_dir}/build/flasher_args.json" \
  --artifact-url "${artifact_url}" \
  --manifest "${repo_root}/dist/manifest-mrdiy-esp32-v13.json" \
  --recovery-report "${repo_root}/dist/recovery-validation-mrdiy-esp32-v13.json" \
  --sdkconfig "${target_dir}/sdkconfig" \
  --chip-family "ESP32" \
  --hardware-family "MRDIY-CAN-SHIELD" \
  --hardware-revision "v1.3+ (RX GPIO 4, TX GPIO 5)" \
  --upstream-tag "main" \
  --source-commit "${source_commit}" \
  --bootloader-offset 0x1000 \
  --listen-only-source-dir "${target_dir}/main" \
  --status-source-dir "${target_dir}/main" \
  --physical-recovery-status "${physical_recovery_status}"

(
  cd "${repo_root}/dist"
  sha256sum "${artifact}" "${ota_artifact}" > SHA256SUMS-mrdiy-esp32-v13
)
