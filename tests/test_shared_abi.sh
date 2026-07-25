#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rl-c-client-abi.XXXXXX")"
ACTUAL_SYMBOLS="$BUILD_DIR/actual.symbols"

cleanup() {
  rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

cmake \
  -S "$ROOT" \
  -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DRCLIENT_VERSION=2.4.6 \
  -DRCLIENT_BUILD_STATIC=OFF \
  -DRCLIENT_BUILD_TESTS=OFF \
  -DRCLIENT_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD_DIR" --parallel 2

case "$(uname -s)" in
  Darwin)
    LIBRARY="$BUILD_DIR/librclient.2.4.6.dylib"
    nm -gU "$LIBRARY" |
      awk '$2 ~ /^[TDSB]$/ {print $3}' |
      sed 's/^_//' |
      sort > "$ACTUAL_SYMBOLS"
    otool -D "$LIBRARY" | grep -Fq '@rpath/librclient.2.dylib'
    ;;
  *)
    LIBRARY="$BUILD_DIR/librclient.so.2.4.6"
    nm -D --defined-only "$LIBRARY" |
      awk '$2 ~ /^[TDSB]$/ {print $3}' |
      sort > "$ACTUAL_SYMBOLS"
    readelf -d "$LIBRARY" | grep -Fq 'Library soname: [librclient.so.2]'
    ;;
esac

diff -u "$ROOT/manifest/public-api.symbols" "$ACTUAL_SYMBOLS"
