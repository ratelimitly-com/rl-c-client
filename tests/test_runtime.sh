#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/tests/production_p0_profile.sh"
PORT="${R_RUNTIME_TEST_PORT:-39085}"
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

"$ROOT/bin/r_test_responder" \
  "--listen=127.0.0.1:$PORT" \
  --scenario=guard-pass \
  --auth=aes \
  --steering=rebind \
  >"$TMP_DIR/responder.out" 2>"$TMP_DIR/responder.err" &
RESPONDER_PID=$!

for _ in {1..100}; do
  grep -q '"event":"ready"' "$TMP_DIR/responder.out" && break
  if ! kill -0 "$RESPONDER_PID" 2>/dev/null; then
    sed -n '1,80p' "$TMP_DIR/responder.err" >&2
    exit 1
  fi
  sleep 0.01
done

grep -q '"event":"ready"' "$TMP_DIR/responder.out"
if ! "$ROOT/tests/test_runtime" "$PORT" 2>"$TMP_DIR/runtime.err"; then
  sed -n '1,160p' "$TMP_DIR/runtime.err" >&2
  exit 1
fi
production_p0_report_profiles \
    "$TMP_DIR/runtime.err" test_runtime 2 true
sleep 0.1
kill -0 "$RESPONDER_PID" 2>/dev/null
kill -TERM "$RESPONDER_PID"
wait "$RESPONDER_PID"
RESPONDER_PID=""

rate_count="$(grep -c '"event":"rate_request"' "$TMP_DIR/responder.out" || true)"
((rate_count >= 2 && rate_count <= 8))
[[ "$(grep -c '"event":"latency_report"' "$TMP_DIR/responder.out" || true)" -eq 1 ]]
grep -q '"event":"rate_request".*"guards":1.*"resources":1' \
  "$TMP_DIR/responder.out"
grep -q '"event":"latency_report".*"reports":1' "$TMP_DIR/responder.out"
