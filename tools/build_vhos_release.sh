#!/usr/bin/env bash
set -euo pipefail

release="${1:-v0.1.0-dev.1}"
artifact="vhos-wican-pro-esp32s3-${release}-merged.bin"
artifact_url="${VHOS_ARTIFACT_URL:-https://github.com/IsaiahDupree/4runner-vhos-firmware/releases/download/${release}/${artifact}}"

idf.py build
mkdir -p dist
idf.py merge-bin --format raw -o "${PWD}/dist/${artifact}"

project_name="$(${PYTHON:-python3} -c 'import json; print(json.load(open("build/project_description.json", encoding="utf-8"))["project_name"])')"
${PYTHON:-python3} tools/validate_vhos_release.py \
  --release "${release}" \
  --merged "dist/${artifact}" \
  --app "build/${project_name}.bin" \
  --partition-table build/partition_table/partition-table.bin \
  --flasher-args build/flasher_args.json \
  --artifact-url "${artifact_url}" \
  --manifest dist/manifest.json \
  --recovery-report dist/recovery-validation.json

sha256sum "dist/${artifact}" "build/${project_name}.bin" > dist/SHA256SUMS
