#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rl-c-client-cmake.XXXXXX")"

cleanup() {
  rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

for manifest in core workflow runtime; do
  test -s "$ROOT/manifest/$manifest.sources"
done

cmake \
  -S "$ROOT" \
  -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DRCLIENT_BUILD_TESTS=ON \
  -DRCLIENT_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD_DIR" --parallel 2
ctest --test-dir "$BUILD_DIR" --output-on-failure

test -s "$BUILD_DIR/librclient.a"
case "$(uname -s)" in
  Darwin)
    test -s "$BUILD_DIR/librclient.dylib"
    ;;
  *)
    test -s "$BUILD_DIR/librclient.so"
    ;;
esac

grep -Fq 'manifest/core.sources' "$ROOT/Makefile"
grep -Fq 'manifest/workflow.sources' "$ROOT/Makefile"
grep -Fq 'manifest/runtime.sources' "$ROOT/Makefile"
