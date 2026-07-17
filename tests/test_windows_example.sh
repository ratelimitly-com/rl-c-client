#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINGW_CC="${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || true)}"
MINGW_AR="${MINGW_AR:-$(command -v x86_64-w64-mingw32-ar || true)}"
OPENSSL_PREFIX="${MINGW_OPENSSL_PREFIX:-}"
WINDOWS_RUNNER="${WINDOWS_RUNNER:-}"
PREBUILT_EXAMPLE="${WINDOWS_EXAMPLE_BINARY:-}"
PORT="${R_WINDOWS_EXAMPLE_TEST_PORT:-39130}"

skip() {
  echo "test_windows_example: SKIP ($*)"
  exit 0
}

OPENSSL_LIBDIR=""
if [[ -z "$PREBUILT_EXAMPLE" ]]; then
  [[ -n "$MINGW_CC" && -n "$MINGW_AR" ]] \
    || skip "MinGW-w64 compiler and archiver are required"
  [[ -n "$OPENSSL_PREFIX" && -f "$OPENSSL_PREFIX/include/openssl/evp.h" ]] \
    || skip "set MINGW_OPENSSL_PREFIX to a Windows OpenSSL installation"

  if [[ -f "$OPENSSL_PREFIX/lib64/libcrypto.a" ]]; then
    OPENSSL_LIBDIR="$OPENSSL_PREFIX/lib64"
  elif [[ -f "$OPENSSL_PREFIX/lib/libcrypto.a" ]]; then
    OPENSSL_LIBDIR="$OPENSSL_PREFIX/lib"
  else
    skip "MINGW_OPENSSL_PREFIX has no static libcrypto.a"
  fi
elif [[ ! -s "$PREBUILT_EXAMPLE" ]]; then
  echo "test_windows_example: WINDOWS_EXAMPLE_BINARY is not readable" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/r-win32-example.XXXXXX")"
RESPONDER_PID=""
EXAMPLE="${PREBUILT_EXAMPLE:-$ROOT/examples/win32/win32-example.exe}"
BUILT_EXAMPLE=false

cleanup() {
  if [[ -n "$RESPONDER_PID" ]] && kill -0 "$RESPONDER_PID" 2>/dev/null; then
    kill -TERM "$RESPONDER_PID" 2>/dev/null || true
    wait "$RESPONDER_PID" 2>/dev/null || true
  fi
  if $BUILT_EXAMPLE; then
    make -C "$ROOT/examples/win32" clean >/dev/null
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

compile_client() {
  local sources=(
    r_client
    r_client_runtime
    r_client_workflow
    r_protocol
    r_crypto
    r_policy
  )
  for source in "${sources[@]}"; do
    "$MINGW_CC" -std=c11 -Wall -Wextra -Werror \
      -I"$ROOT/include" -I"$ROOT/src" -I"$OPENSSL_PREFIX/include" \
      -c "$ROOT/src/$source.c" -o "$TMP_DIR/$source.o"
  done
  "$MINGW_AR" rcs "$TMP_DIR/librclient.a" "$TMP_DIR"/*.o
}

link_example() {
  make -C "$ROOT/examples/win32" clean >/dev/null
  make -C "$ROOT/examples/win32" \
    CC="$MINGW_CC" \
    OPENSSL_PREFIX="$OPENSSL_PREFIX" \
    OPENSSL_LIBDIR="$OPENSSL_LIBDIR" \
    RL_CLIENT_LIBRARY="$TMP_DIR/librclient.a" \
    >/dev/null
  [[ -s "$EXAMPLE" ]] \
    || { echo "test_windows_example: PE executable was not created" >&2; exit 1; }
  BUILT_EXAMPLE=true
}

find_runner() {
  [[ -n "$WINDOWS_RUNNER" ]] && return
  for candidate in wine64 wine /usr/lib/wine/wine64; do
    if command -v "$candidate" >/dev/null 2>&1; then
      WINDOWS_RUNNER="$(command -v "$candidate")"
      return
    fi
  done
}

wait_for_responder() {
  local output=$1
  for _ in {1..100}; do
    grep -q '"event":"ready"' "$output" && return
    if ! kill -0 "$RESPONDER_PID" 2>/dev/null; then
      echo "test_windows_example: responder exited before readiness" >&2
      exit 1
    fi
    sleep 0.01
  done
  echo "test_windows_example: responder did not become ready" >&2
  exit 1
}

run_scenario() {
  local scenario=$1
  local scenario_port=$2
  local max_packets=$3
  local expected_status=$4
  local directory="$TMP_DIR/$scenario"
  mkdir -p "$directory"

  "$ROOT/bin/r_test_responder" \
    "--listen=127.0.0.1:$scenario_port" \
    "--scenario=$scenario" \
    --auth=aes \
    "--max-packets=$max_packets" \
    >"$directory/responder.out" 2>"$directory/responder.err" &
  RESPONDER_PID=$!
  wait_for_responder "$directory/responder.out"

  local key="rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l"
  local runner_args=()
  read -r -a runner_args <<<"${WINDOWS_RUNNER_ARGS:-}"
  set +e
  WINEPREFIX="${WINEPREFIX:-$TMP_DIR/wine-prefix}" \
  WINEDEBUG="${WINEDEBUG:--all}" \
  RATELIMITLY_TENANT=rn-test.local \
  RATELIMITLY_AUTH_KEY="$key" \
  RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
  RATELIMITLY_EXAMPLE_SERVER_PORT="$scenario_port" \
    "$WINDOWS_RUNNER" "${runner_args[@]}" "$EXAMPLE" \
      >"$directory/example.out" 2>"$directory/example.err"
  local example_status=$?
  set -e
  for _ in {1..100}; do
    ! kill -0 "$RESPONDER_PID" 2>/dev/null && break
    sleep 0.01
  done
  if kill -0 "$RESPONDER_PID" 2>/dev/null; then
    kill -TERM "$RESPONDER_PID" 2>/dev/null || true
    wait "$RESPONDER_PID" 2>/dev/null || true
    RESPONDER_PID=""
    sed -n '1,80p' "$directory/example.err" >&2
    echo "test_windows_example: $scenario did not send all packets" >&2
    exit 1
  fi
  wait "$RESPONDER_PID"
  RESPONDER_PID=""

  [[ "$example_status" -eq "$expected_status" ]] || {
    sed -n '1,80p' "$directory/example.err" >&2
    echo "test_windows_example: $scenario exited $example_status" >&2
    exit 1
  }
}

if [[ -z "$PREBUILT_EXAMPLE" ]]; then
  compile_client
  link_example
fi
find_runner
if [[ -z "$WINDOWS_RUNNER" ]]; then
  echo "test_windows_example: PASS (strict Windows compile/link; no runner)"
  exit 0
fi

make -C "$ROOT" test-responder >/dev/null
run_scenario guard-pass "$PORT" 2 0
grep -q '^allowed: .*latency=' "$TMP_DIR/guard-pass/example.out"
grep -q '"event":"rate_request".*"guards":1.*"resources":1' \
  "$TMP_DIR/guard-pass/responder.out"
grep -q '"event":"latency_report".*"reports":1' \
  "$TMP_DIR/guard-pass/responder.out"

run_scenario guard-deny "$((PORT + 1))" 1 2
grep -q '^denied: latency guard' "$TMP_DIR/guard-deny/example.out"
if grep -q '"event":"latency_report"' "$TMP_DIR/guard-deny/responder.out"; then
  echo "test_windows_example: denied work emitted a latency report" >&2
  exit 1
fi

echo "test_windows_example: PASS (Wine allowed and denied paths)"
