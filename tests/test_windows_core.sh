#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINGW_CC="${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || true)}"

if [[ -z "$MINGW_CC" ]]; then
  echo "test_windows_core: SKIP (x86_64-w64-mingw32-gcc not installed)"
  exit 0
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/r-core-win32.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

for source in r_client.c r_client_workflow.c r_policy.c r_protocol.c; do
  "$MINGW_CC" -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/tests/win32_stubs" -I"$ROOT/include" -I"$ROOT/src" \
    -c "$ROOT/src/$source" -o "$TMP_DIR/${source%.c}.o"
done
