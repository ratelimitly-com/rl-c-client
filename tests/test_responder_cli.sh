#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESPONDER="$ROOT/bin/r_test_responder"
PORT="${R_TEST_RESPONDER_PORT:-39081}"
TMP_DIR="$(mktemp -d)"
RESPONDER_PID=""

cleanup() {
  if [[ -n "$RESPONDER_PID" ]] && kill -0 "$RESPONDER_PID" 2>/dev/null; then
    kill -TERM "$RESPONDER_PID" 2>/dev/null || true
    wait "$RESPONDER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

fail() {
  echo "test_responder_cli: $*" >&2
  exit 1
}

expect_start_and_stop() {
  local scenario="$1"
  local auth="$2"
  local output="$TMP_DIR/${scenario}-${auth}.out"
  local error="$TMP_DIR/${scenario}-${auth}.err"

  "$RESPONDER" \
    "--listen=127.0.0.1:$PORT" \
    "--scenario=$scenario" \
    "--auth=$auth" \
    --delay-ms=1 \
    >"$output" 2>"$error" &
  RESPONDER_PID=$!

  local ready=0
  for _ in {1..100}; do
    if grep -q '"event":"ready"' "$output"; then
      ready=1
      break
    fi
    if ! kill -0 "$RESPONDER_PID" 2>/dev/null; then
      break
    fi
    sleep 0.01
  done
  if [[ "$ready" -ne 1 ]]; then
    sed -n '1,40p' "$error" >&2
    fail "$scenario/$auth did not become ready"
  fi

  if [[ "$scenario" == "allow" && "$auth" == "aes" ]]; then
    printf 'invalid' >"/dev/udp/127.0.0.1/$PORT"
    local rejected=0
    for _ in {1..100}; do
      if grep -q '"event":"input_rejected"' "$output"; then
        rejected=1
        break
      fi
      sleep 0.01
    done
    [[ "$rejected" -eq 1 ]] || fail "live malformed input was not rejected"
  fi

  kill -TERM "$RESPONDER_PID"
  wait "$RESPONDER_PID"
  RESPONDER_PID=""

  [[ "$(grep -c '"event":"ready"' "$output")" -eq 1 ]] \
    || fail "$scenario/$auth emitted an invalid readiness stream"
}

"$RESPONDER" --help >"$TMP_DIR/help"
grep -q -- '--scenario=<name>' "$TMP_DIR/help" \
  || fail "help does not document scenarios"

"$RESPONDER" --print-nginx-config >"$TMP_DIR/config"
grep -q 'Synthetic rl-c-client test responder' "$TMP_DIR/config" \
  || fail "generated config is not marked synthetic"
grep -q '^ratelimitly_auth_key rl-aes' "$TMP_DIR/config" \
  || fail "generated config does not contain the AES fixture"

if "$RESPONDER" --print-nginx-config --listen=0.0.0.0:"$PORT" \
    >"$TMP_DIR/wildcard-config.out" 2>"$TMP_DIR/wildcard-config.err"; then
  fail "config generation accepted a wildcard listen address"
fi
if [[ -s "$TMP_DIR/wildcard-config.out" ]]; then
  fail "invalid config generation emitted nginx configuration"
fi

if "$RESPONDER" --listen=0.0.0.0:"$PORT" >"$TMP_DIR/wildcard.out" 2>"$TMP_DIR/wildcard.err"; then
  fail "wildcard listen address was accepted"
fi
if grep -q '"event":"ready"' "$TMP_DIR/wildcard.out"; then
  fail "invalid configuration emitted a readiness record"
fi

scenarios=(
  allow
  deny
  guard-pass
  guard-deny
  quota
  drop
  malformed-auth
  malformed-truncated
  malformed-request-id
  count-empty
  count-short
  count-extra
)

for scenario in "${scenarios[@]}"; do
  expect_start_and_stop "$scenario" aes
done
expect_start_and_stop allow cookie
