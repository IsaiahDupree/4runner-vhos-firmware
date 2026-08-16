#!/usr/bin/env bash
set -euo pipefail

timeout_seconds="${1:-20}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${VHOS_BLE_PROBE_BUILD_ROOT:-/tmp/vhos-ble-probe-stable}"
app_bundle="${build_root}/VHOSBLEProbe.app"
executable="${app_bundle}/Contents/MacOS/VHOSBLEProbe"
log_file="${build_root}/probe.log"

mkdir -p "${app_bundle}/Contents/MacOS"
if [[ ! -x "${executable}" \
  || "${repo_root}/tools/vhos_ble_probe.swift" -nt "${executable}" \
  || "${repo_root}/tools/VHOSBLEProbe-Info.plist" -nt "${app_bundle}/Contents/Info.plist" ]]; then
  install -m 0644 "${repo_root}/tools/VHOSBLEProbe-Info.plist" "${app_bundle}/Contents/Info.plist"
  swiftc -swift-version 5 "${repo_root}/tools/vhos_ble_probe.swift" -o "${executable}"
  codesign --force --sign - "${app_bundle}" >/dev/null
fi
: > "${log_file}"
open -n -W "${app_bundle}" --args "${timeout_seconds}" "${log_file}"
cat "${log_file}"
grep -q "VHOS_BLE_PROBE_PASS" "${log_file}"
