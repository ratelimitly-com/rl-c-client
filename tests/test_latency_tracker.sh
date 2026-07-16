#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${R_LATENCY_EXAMPLE_TEST_PORT:-39083}"
DENY_PORT=$((PORT + 1))
TMP_DIR="$(mktemp -d)"
RESPONDER_PID=""
TEST_AES_KEY="rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l"

cleanup() {
  if [[ -n "$RESPONDER_PID" ]] && kill -0 "$RESPONDER_PID" 2>/dev/null; then
    kill -TERM "$RESPONDER_PID" 2>/dev/null || true
    wait "$RESPONDER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

"$ROOT/bin/r_test_responder" \
  "--listen=127.0.0.1:$PORT" \
  --scenario=guard-pass \
  --auth=aes \
  --max-packets=2 \
  >"$TMP_DIR/responder.out" 2>"$TMP_DIR/responder.err" &
RESPONDER_PID=$!

for _ in {1..100}; do
  if grep -q '"event":"ready"' "$TMP_DIR/responder.out"; then
    break
  fi
  if ! kill -0 "$RESPONDER_PID" 2>/dev/null; then
    sed -n '1,80p' "$TMP_DIR/responder.err" >&2
    exit 1
  fi
  sleep 0.01
done

grep -q '"event":"ready"' "$TMP_DIR/responder.out"
RATELIMITLY_TENANT=rn-test.local \
RATELIMITLY_AUTH_KEY="$TEST_AES_KEY" \
RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
RATELIMITLY_EXAMPLE_SERVER_PORT="$PORT" \
RATELIMITLY_EXAMPLE_WORK_MS=1 \
  "$ROOT/tests/test_latency_tracker" >"$TMP_DIR/example.out"

wait "$RESPONDER_PID"
RESPONDER_PID=""
grep -q '"event":"rate_request".*"guards":1.*"resources":1' \
  "$TMP_DIR/responder.out"
grep -q '"event":"latency_report".*"reports":1' "$TMP_DIR/responder.out"
grep -q '^guard passed: current=' "$TMP_DIR/example.out"
grep -q '^latency reported: service=example-inventory-backend observed=' \
  "$TMP_DIR/example.out"

# A failed guard means the protected operation never ran. Reporting a zero or
# synthetic latency for that path would corrupt the service's tracker data.
"$ROOT/bin/r_test_responder" \
  "--listen=127.0.0.1:$DENY_PORT" \
  --scenario=guard-deny \
  --auth=aes \
  --max-packets=1 \
  >"$TMP_DIR/deny-responder.out" 2>"$TMP_DIR/deny-responder.err" &
RESPONDER_PID=$!

for _ in {1..100}; do
  if grep -q '"event":"ready"' "$TMP_DIR/deny-responder.out"; then
    break
  fi
  if ! kill -0 "$RESPONDER_PID" 2>/dev/null; then
    sed -n '1,80p' "$TMP_DIR/deny-responder.err" >&2
    exit 1
  fi
  sleep 0.01
done

grep -q '"event":"ready"' "$TMP_DIR/deny-responder.out"
RATELIMITLY_TENANT=rn-test.local \
RATELIMITLY_AUTH_KEY="$TEST_AES_KEY" \
RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
RATELIMITLY_EXAMPLE_SERVER_PORT="$DENY_PORT" \
RATELIMITLY_EXAMPLE_WORK_MS=1 \
  "$ROOT/tests/test_latency_tracker" >"$TMP_DIR/deny-example.out"

wait "$RESPONDER_PID"
RESPONDER_PID=""
grep -q '"event":"rate_request".*"guards":1.*"resources":1' \
  "$TMP_DIR/deny-responder.out"
if grep -q '"event":"latency_report"' "$TMP_DIR/deny-responder.out"; then
  echo "denied latency guard unexpectedly emitted a report" >&2
  exit 1
fi
grep -q '^guard failed: current=' "$TMP_DIR/deny-example.out"
if grep -q '^latency reported:' "$TMP_DIR/deny-example.out"; then
  echo "denied latency guard unexpectedly performed protected work" >&2
  exit 1
fi
