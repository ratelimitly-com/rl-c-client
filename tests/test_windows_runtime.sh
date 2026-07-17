#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINGW_CC="${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || true)}"

if [[ -z "$MINGW_CC" ]]; then
  echo "test_windows_runtime: SKIP (x86_64-w64-mingw32-gcc not installed)"
  exit 0
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/r-runtime-win32.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

grep -Fq 'RL_CLIENT_LIBRARY ?=' "$ROOT/examples/win32/Makefile" \
  || { echo "test_windows_runtime: Win32 build cannot select its target library" >&2; exit 1; }

"$MINGW_CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  -c "$ROOT/src/r_client_runtime.c" -o "$TMP_DIR/runtime.o"
"$MINGW_CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  -c "$ROOT/examples/win32/main.c" -o "$TMP_DIR/example.o"
