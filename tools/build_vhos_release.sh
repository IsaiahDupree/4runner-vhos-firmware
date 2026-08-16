#!/usr/bin/env bash
set -euo pipefail

release="${1:-v0.1.0-dev.1}"
artifact="vhos-wican-pro-esp32s3-${release}-merged.bin"
ota_artifact="vhos-wican-pro-esp32s3-${release}-ota.bin"
artifact_url="${VHOS_ARTIFACT_URL:-https://github.com/IsaiahDupree/4runner-vhos-firmware/releases/download/${release}/${artifact}}"

if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "release build requires a clean tracked worktree" >&2
  exit 1
fi

# ESP-IDF normalizes local-component paths in dependencies.lock to the current
# absolute checkout. Preserve the reviewed lockfile so that path-only build
# metadata cannot make the release validator report false source drift.
lock_backup="$(mktemp)"
cp dependencies.lock "${lock_backup}"
restore_dependency_lock() {
  cp "${lock_backup}" dependencies.lock
  rm -f "${lock_backup}"
}
trap restore_dependency_lock EXIT

# Git metadata is embedded in both the project name and VHOS handshake. Force
# CMake to observe the current commit instead of reusing a prior build cache.
idf.py reconfigure
idf.py build
mkdir -p dist
idf.py merge-bin --format raw -o "${PWD}/dist/${artifact}"
restore_dependency_lock
trap - EXIT

project_name="$(${PYTHON:-python3} -c 'import json; print(json.load(open("build/project_description.json", encoding="utf-8"))["project_name"])')"
install -m 0644 "build/${project_name}.bin" "dist/${ota_artifact}"
${PYTHON:-python3} tools/validate_vhos_release.py \
  --release "${release}" \
  --merged "dist/${artifact}" \
  --app "build/${project_name}.bin" \
  --partition-table build/partition_table/partition-table.bin \
  --flasher-args build/flasher_args.json \
  --artifact-url "${artifact_url}" \
  --manifest dist/manifest.json \
  --recovery-report dist/recovery-validation.json

sha256sum "dist/${artifact}" "dist/${ota_artifact}" > dist/SHA256SUMS
