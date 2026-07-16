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
  local host="${3:-127.0.0.1}"
  local case_name="${scenario}-${auth}-${host//:/_}"
  local output="$TMP_DIR/${case_name}.out"
  local error="$TMP_DIR/${case_name}.err"

  "$RESPONDER" \
    "--listen=$host:$PORT" \
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
  grep -Fq "\"address\":\"$host\"" "$output" \
    || fail "$scenario/$auth reported the wrong bind address"

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

"$RESPONDER" --print-nginx-config --listen=127.0.0.1:"$PORT" \
  >"$TMP_DIR/config"
grep -q 'Synthetic rl-c-client test responder' "$TMP_DIR/config" \
  || fail "generated config is not marked synthetic"
grep -q '^ratelimitly_auth_key rl-aes' "$TMP_DIR/config" \
  || fail "generated config does not contain the AES fixture"

if "$RESPONDER" --print-nginx-config \
    >"$TMP_DIR/missing-listen.out" 2>"$TMP_DIR/missing-listen.err"; then
  fail "config generation accepted a missing listen address"
fi
if [[ -s "$TMP_DIR/missing-listen.out" ]]; then
  fail "missing listen address emitted nginx configuration"
fi

"$RESPONDER" --print-nginx-config --listen=0.0.0.0:"$PORT" \
  >"$TMP_DIR/wildcard-config.out"
grep -Fq "# responder=0.0.0.0:$PORT" "$TMP_DIR/wildcard-config.out" \
  || fail "config generation did not preserve the explicit wildcard address"

"$RESPONDER" --print-nginx-config --listen=192.0.2.1:"$PORT" \
  >"$TMP_DIR/nonlocal-config.out"
grep -Fq "# responder=192.0.2.1:$PORT" "$TMP_DIR/nonlocal-config.out" \
  || fail "config generation rejected an explicit nonlocal address"

"$RESPONDER" --print-nginx-config "--listen=[::]:$PORT" \
  >"$TMP_DIR/ipv6-wildcard-config.out"
grep -Fq "# responder=[::]:$PORT" "$TMP_DIR/ipv6-wildcard-config.out" \
  || fail "config generation rejected an explicit IPv6 wildcard address"

if "$RESPONDER" --print-nginx-config --listen=not-an-ip:"$PORT" \
    >"$TMP_DIR/invalid-config.out" 2>"$TMP_DIR/invalid-config.err"; then
  fail "config generation accepted a non-numeric listen address"
fi
if [[ -s "$TMP_DIR/invalid-config.out" ]]; then
  fail "invalid config generation emitted nginx configuration"
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
expect_start_and_stop allow aes 0.0.0.0
