#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$(mktemp "${TMPDIR:-/tmp}/vhos-ble-transfer-policy.XXXXXX")"
trap 'rm -f "${binary}"' EXIT

"${CC:-cc}" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -pedantic \
  -I"${repo_root}/targets/mrdiy-esp32-v13/main" \
  "${repo_root}/tools/test_mrdiy_ble_transfer_policy.c" \
  -o "${binary}"

"${binary}"
