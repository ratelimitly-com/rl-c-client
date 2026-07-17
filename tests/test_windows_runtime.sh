#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINGW_CC="${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || true)}"

if [[ -z "$MINGW_CC" ]]; then
  echo "test_windows_runtime: SKIP (x86_64-w64-mingw32-gcc not installed)"
  exit 0
fi

OBJECT="$(mktemp "${TMPDIR:-/tmp}/r-runtime-win32.XXXXXX.o")"
trap 'rm -f "$OBJECT"' EXIT

"$MINGW_CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  -c "$ROOT/src/r_client_runtime.c" -o "$OBJECT"
