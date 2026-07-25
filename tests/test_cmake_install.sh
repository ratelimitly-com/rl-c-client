#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/rl-c-client-install.XXXXXX")"
BUILD_DIR="$TMP_ROOT/build"
PREFIX="$TMP_ROOT/prefix"
CONSUMER_BUILD="$TMP_ROOT/consumer-build"

cleanup() {
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

cmake \
  -S "$ROOT" \
  -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DRCLIENT_VERSION=1.2.3 \
  -DRCLIENT_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR" --parallel 2
cmake --install "$BUILD_DIR"

for header in \
  r_client.h \
  r_client_export.h \
  r_client_io.h \
  r_client_runtime.h \
  r_client_workflow.h; do
  test -s "$PREFIX/include/$header"
done

test -s "$PREFIX/lib/librclient.a"
test -s "$PREFIX/lib/pkgconfig/rclient.pc"
test -s "$PREFIX/lib/cmake/rclient/rclient-config.cmake"
test -s "$PREFIX/lib/cmake/rclient/rclient-targets.cmake"

case "$(uname -s)" in
  Darwin)
    test -s "$PREFIX/lib/librclient.1.dylib"
    export DYLD_LIBRARY_PATH="$PREFIX/lib"
    ;;
  *)
    test -s "$PREFIX/lib/librclient.so.1"
    export LD_LIBRARY_PATH="$PREFIX/lib"
    ;;
esac

cmake \
  -S "$ROOT/tests/fixtures/installed_consumer" \
  -B "$CONSUMER_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build "$CONSUMER_BUILD" --parallel 2
"$CONSUMER_BUILD/rclient-installed-consumer"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
cc \
  "$ROOT/tests/fixtures/installed_consumer/main.c" \
  $(pkg-config --cflags --libs rclient) \
  -o "$TMP_ROOT/pkg-config-consumer"
"$TMP_ROOT/pkg-config-consumer"
