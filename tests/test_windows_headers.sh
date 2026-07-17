#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"

if ! command -v "$CC" >/dev/null 2>&1; then
  echo "test_windows_headers: SKIP ($CC not installed)"
  exit 0
fi

output="$(mktemp -t rl-windows-headers.XXXXXX.o)"
trap 'rm -f "$output"' EXIT

"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  -c "$ROOT/tests/test_windows_headers.c" -o "$output"
