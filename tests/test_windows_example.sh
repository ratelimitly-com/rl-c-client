#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINGW_CC="${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || true)}"
MINGW_AR="${MINGW_AR:-$(command -v x86_64-w64-mingw32-ar || true)}"
OPENSSL_PREFIX="${MINGW_OPENSSL_PREFIX:-}"
WINDOWS_RUNNER="${WINDOWS_RUNNER:-}"
PREBUILT_EXAMPLE="${WINDOWS_EXAMPLE_BINARY:-}"
PORT="${R_WINDOWS_EXAMPLE_TEST_PORT:-39130}"
TEST_AES_KEY="rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l"
TRACKER_JSON='"tracker":{"ttl_ms":10000,"max_samples":100,"buffer_size":32,"min_sample_threshold":5}'

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
EXAMPLE_PID=""
EXAMPLE="${PREBUILT_EXAMPLE:-$ROOT/examples/win32/win32-example.exe}"
BUILT_EXAMPLE=false

cleanup() {
  if [[ -n "$EXAMPLE_PID" ]] && kill -0 "$EXAMPLE_PID" 2>/dev/null; then
    kill -TERM "$EXAMPLE_PID" 2>/dev/null || true
    wait "$EXAMPLE_PID" 2>/dev/null || true
  fi
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

wait_with_deadline() {
  local pid=$1
  local seconds=$2
  local attempts=$((seconds * 100))
  local attempt=0
  while kill -0 "$pid" 2>/dev/null && ((attempt < attempts)); do
    sleep 0.01
    attempt=$((attempt + 1))
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    sleep 1
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    return 124
  fi
  wait "$pid"
}

count_events() {
  local event=$1
  local file=$2
  grep -c "\"event\":\"$event\"" "$file" 2>/dev/null || true
}

fail_case() {
  local scenario=$1
  local message=$2
  local directory="$TMP_DIR/$scenario"
  echo "test_windows_example: $scenario: $message" >&2
  local file
  for file in example.out example.err responder.out responder.err; do
    if [[ -s "$directory/$file" ]]; then
      echo "--- $file" >&2
      sed -n '1,100p' "$directory/$file" >&2
    fi
  done
  exit 1
}

assert_outputs() {
  local scenario=$1
  local expected_status=$2
  local actual_status=$3
  local directory="$TMP_DIR/$scenario"
  local responder="$directory/responder.out"
  local example="$directory/example.out"
  local expected_reports=0
  local rate_line

  [[ "$actual_status" -eq "$expected_status" ]] \
    || fail_case "$scenario" \
      "example exited $actual_status; expected $expected_status"
  case "$scenario" in
    guard-pass)
      expected_reports=1
      grep -Eq '^allowed: .*latency=[0-9]+' "$example" \
        || fail_case "$scenario" "allowed work or latency output missing"
      ;;
    deny)
      grep -Fq 'denied: resource rate limit' "$example" \
        || fail_case "$scenario" "resource denial output missing"
      ;;
    guard-deny)
      grep -Fq 'denied: latency guard' "$example" \
        || fail_case "$scenario" "latency denial output missing"
      ;;
  esac
  if [[ "$scenario" != "guard-pass" ]] && grep -Fq 'allowed:' "$example"; then
    fail_case "$scenario" "denied path exposed protected work"
  fi

  [[ "$(count_events rate_request "$responder")" -eq 1 ]] \
    || fail_case "$scenario" "expected exactly one rate request"
  [[ "$(count_events latency_report "$responder")" -eq "$expected_reports" ]] \
    || fail_case "$scenario" \
      "expected $expected_reports latency report(s)"
  [[ "$(count_events input_rejected "$responder")" -eq 0 ]] \
    || fail_case "$scenario" "responder rejected an input packet"
  rate_line="$(grep '"event":"rate_request"' "$responder")"
  grep -Fq '"guards":1,"resources":1' <<<"$rate_line" \
    || fail_case "$scenario" "request omitted resource or latency admission"
  grep -Fq '"label":"win32-example"' <<<"$rate_line" \
    || fail_case "$scenario" "request used the wrong metrics label"
  grep -Fq "$TRACKER_JSON" <<<"$rate_line" \
    || fail_case "$scenario" "guard tracker configuration changed"
  grep -Fq '"guard_threshold_ms":100' <<<"$rate_line" \
    || fail_case "$scenario" "latency threshold changed"
  grep -Fq "\"disposition\":\"$scenario\"" <<<"$rate_line" \
    || fail_case "$scenario" "responder observed the wrong scenario"

  if [[ "$scenario" == "guard-pass" ]]; then
    local latency_line
    latency_line="$(grep '"event":"latency_report"' "$responder")"
    grep -Fq '"reports":1' <<<"$latency_line" \
      || fail_case "$scenario" "latency packet did not contain one report"
    grep -Fq "$TRACKER_JSON" <<<"$latency_line" \
      || fail_case "$scenario" "reported tracker configuration changed"
    grep -Eq '"observed_latency_ms":[0-9]+' <<<"$latency_line" \
      || fail_case "$scenario" "latency observation missing"
    grep -Fq '"matches_previous_guard":true' <<<"$latency_line" \
      || fail_case "$scenario" "report targeted a different tracker"
  fi
}

run_scenario() {
  local scenario=$1
  local scenario_port=$2
  local expected_status=$3
  local directory="$TMP_DIR/$scenario"
  mkdir -p "$directory"

  "$ROOT/bin/r_test_responder" \
    "--listen=127.0.0.1:$scenario_port" \
    "--scenario=$scenario" \
    --auth=aes \
    >"$directory/responder.out" 2>"$directory/responder.err" &
  RESPONDER_PID=$!
  wait_for_responder "$directory/responder.out"

  local runner_args=()
  read -r -a runner_args <<<"${WINDOWS_RUNNER_ARGS:-}"
  (
    unset RATELIMITLY_TENANT
    exec env \
      WINEPREFIX="${WINEPREFIX:-$TMP_DIR/wine-prefix}" \
      WINEDEBUG="${WINEDEBUG:--all}" \
      RATELIMITLY_AUTH_KEY="$TEST_AES_KEY" \
      RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
      RATELIMITLY_EXAMPLE_SERVER_PORT="$scenario_port" \
      "$WINDOWS_RUNNER" "${runner_args[@]}" "$EXAMPLE"
  ) >"$directory/example.out" 2>"$directory/example.err" &
  EXAMPLE_PID=$!
  local example_status=0
  wait_with_deadline "$EXAMPLE_PID" 30 || example_status=$?
  EXAMPLE_PID=""
  [[ "$example_status" -ne 124 ]] \
    || fail_case "$scenario" "example timed out"

  # Keep the responder alive after the executable returns. A bounded drain
  # makes forbidden late reports and duplicate reports observable.
  sleep 0.1
  kill -0 "$RESPONDER_PID" 2>/dev/null \
    || fail_case "$scenario" "responder exited before packet drain"
  kill -TERM "$RESPONDER_PID"
  local responder_status=0
  wait_with_deadline "$RESPONDER_PID" 5 || responder_status=$?
  RESPONDER_PID=""
  [[ "$responder_status" -eq 0 ]] \
    || fail_case "$scenario" "responder exited $responder_status"

  assert_outputs "$scenario" "$expected_status" "$example_status"
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
run_scenario guard-pass "$PORT" 0
run_scenario deny "$((PORT + 1))" 2
run_scenario guard-deny "$((PORT + 2))" 2

echo "test_windows_example: PASS (Wine allow, resource deny, latency deny)"
