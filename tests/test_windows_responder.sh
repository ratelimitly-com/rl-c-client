#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINGW_CC="${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || true)}"
MINGW_AR="${MINGW_AR:-$(command -v x86_64-w64-mingw32-ar || true)}"
OPENSSL_PREFIX="${MINGW_OPENSSL_PREFIX:-}"
WINDOWS_RUNNER="${WINDOWS_RUNNER:-}"
PORT="${R_WINDOWS_RESPONDER_TEST_PORT:-39120}"
TEST_AES_KEY="rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l"

skip() {
  echo "test_windows_responder: SKIP ($*)"
  exit 0
}

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

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/r-responder-win32.XXXXXX")"
RESPONDER_PID=""
CLIENT_PID=""

cleanup() {
  if [[ -n "$CLIENT_PID" ]] && kill -0 "$CLIENT_PID" 2>/dev/null; then
    kill -TERM "$CLIENT_PID" 2>/dev/null || true
    wait "$CLIENT_PID" 2>/dev/null || true
  fi
  if [[ -n "$RESPONDER_PID" ]] && kill -0 "$RESPONDER_PID" 2>/dev/null; then
    kill -TERM "$RESPONDER_PID" 2>/dev/null || true
    sleep 1
    kill -KILL "$RESPONDER_PID" 2>/dev/null || true
    wait "$RESPONDER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

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

client_sources=(
  r_client
  r_client_runtime
  r_client_workflow
  r_protocol
  r_crypto
  r_policy
)
for source in "${client_sources[@]}"; do
  "$MINGW_CC" -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/include" -I"$ROOT/src" -I"$OPENSSL_PREFIX/include" \
    -c "$ROOT/src/$source.c" -o "$TMP_DIR/$source.o"
done
"$MINGW_AR" rcs "$TMP_DIR/librclient.a" "$TMP_DIR"/r_*.o

for source in r_test_responder r_test_responder_protocol; do
  "$MINGW_CC" -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/include" -I"$ROOT/src" -I"$OPENSSL_PREFIX/include" \
    -c "$ROOT/tools/$source.c" -o "$TMP_DIR/$source.o"
done

"$MINGW_CC" \
  "$TMP_DIR/r_test_responder.o" \
  "$TMP_DIR/r_test_responder_protocol.o" \
  "$TMP_DIR/librclient.a" \
  -L"$OPENSSL_LIBDIR" \
  -lcrypto -lws2_32 -lgdi32 -lcrypt32 -ldnsapi \
  -o "$TMP_DIR/r-test-responder.exe"

[[ -s "$TMP_DIR/r-test-responder.exe" ]] \
  || { echo "test_windows_responder: PE executable was not created" >&2; exit 1; }

if [[ -z "$WINDOWS_RUNNER" ]]; then
  for candidate in wine64 wine /usr/lib/wine/wine64; do
    if command -v "$candidate" >/dev/null 2>&1; then
      WINDOWS_RUNNER="$(command -v "$candidate")"
      break
    fi
  done
fi

if [[ -n "$WINDOWS_RUNNER" ]]; then
  runner_args=()
  read -r -a runner_args <<<"${WINDOWS_RUNNER_ARGS:-}"
  test_wineprefix="${WINEPREFIX:-$TMP_DIR/wine-prefix}"

  "$MINGW_CC" -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/include" -I"$OPENSSL_PREFIX/include" \
    "$ROOT/examples/win32/main.c" \
    "$TMP_DIR/librclient.a" \
    -L"$OPENSSL_LIBDIR" \
    -lcrypto -lws2_32 -lgdi32 -lcrypt32 -ldnsapi \
    -o "$TMP_DIR/win32-example.exe"

  # Initialize Wine/CrossOver before backgrounding the responder. A first-run
  # prefix helper may otherwise outlive its launcher and confuse readiness.
  WINEPREFIX="$test_wineprefix" WINEDEBUG="${WINEDEBUG:--all}" \
    "$WINDOWS_RUNNER" "${runner_args[@]}" cmd /c ver \
    >/dev/null 2>&1 || true

  WINEPREFIX="$test_wineprefix" WINEDEBUG="${WINEDEBUG:--all}" \
    "$WINDOWS_RUNNER" "${runner_args[@]}" \
      "$TMP_DIR/r-test-responder.exe" \
      "--listen=127.0.0.1:$PORT" \
      --scenario=guard-pass \
      --auth=aes \
      --max-packets=2 \
      >"$TMP_DIR/responder.out" 2>"$TMP_DIR/responder.err" &
  RESPONDER_PID=$!

  ready=false
  for _ in {1..500}; do
    if grep -Fq '"event":"ready"' "$TMP_DIR/responder.out"; then
      ready=true
      break
    fi
    kill -0 "$RESPONDER_PID" 2>/dev/null || break
    sleep 0.01
  done
  if ! $ready; then
    sed -n '1,100p' "$TMP_DIR/responder.err" >&2
    echo "test_windows_responder: PE responder did not become ready" >&2
    exit 1
  fi

  (
    unset RATELIMITLY_TENANT
    exec env \
      WINEPREFIX="$test_wineprefix" \
      WINEDEBUG="${WINEDEBUG:--all}" \
      RATELIMITLY_AUTH_KEY="$TEST_AES_KEY" \
      RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
      RATELIMITLY_EXAMPLE_SERVER_PORT="$PORT" \
      "$WINDOWS_RUNNER" "${runner_args[@]}" \
        "$TMP_DIR/win32-example.exe"
  ) >"$TMP_DIR/example.out" 2>"$TMP_DIR/example.err" &
  CLIENT_PID=$!
  client_status=0
  wait_with_deadline "$CLIENT_PID" 30 || client_status=$?
  CLIENT_PID=""
  if [[ "$client_status" -ne 0 ]]; then
    sed -n '1,100p' "$TMP_DIR/example.err" >&2
    echo "test_windows_responder: PE client exited $client_status" >&2
    exit 1
  fi

  responder_status=0
  wait_with_deadline "$RESPONDER_PID" 5 || responder_status=$?
  RESPONDER_PID=""
  if [[ "$responder_status" -ne 0 ]]; then
    sed -n '1,100p' "$TMP_DIR/responder.err" >&2
    echo "test_windows_responder: PE responder exited $responder_status" >&2
    exit 1
  fi

  [[ "$(grep -c '"event":"rate_request"' "$TMP_DIR/responder.out")" -eq 1 ]] \
    || { echo "test_windows_responder: expected one rate request" >&2; exit 1; }
  [[ "$(grep -c '"event":"latency_report"' "$TMP_DIR/responder.out")" -eq 1 ]] \
    || { echo "test_windows_responder: expected one latency report" >&2; exit 1; }
  grep -Fq '"reports":1' "$TMP_DIR/responder.out" \
    || { echo "test_windows_responder: report count changed" >&2; exit 1; }
  grep -Fq '"matches_previous_guard":true' "$TMP_DIR/responder.out" \
    || { echo "test_windows_responder: tracker identity changed" >&2; exit 1; }
  if grep -Fq '"event":"input_rejected"' "$TMP_DIR/responder.out"; then
    echo "test_windows_responder: responder rejected PE client traffic" >&2
    exit 1
  fi
  grep -Eq '^allowed: .*latency=[0-9]+' "$TMP_DIR/example.out" \
    || { echo "test_windows_responder: PE client was not admitted" >&2; exit 1; }

  echo "test_windows_responder: PASS (strict Windows PE roundtrip)"
else
  echo "test_windows_responder: PASS (strict Windows compile/link; no runner)"
fi
