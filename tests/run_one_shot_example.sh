#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESPONDER="$ROOT/bin/r_test_responder"
TEST_AES_KEY="rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l"
TRACKER_JSON='"tracker":{"ttl_ms":10000,"max_samples":100,"buffer_size":32,"min_sample_threshold":5}'

if [[ "$#" -ne 5 ]]; then
  echo "usage: $0 <name> <binary> <profile> <metrics-label> <base-port>" >&2
  exit 2
fi

NAME=$1
BINARY=$2
PROFILE=$3
METRICS_LABEL=$4
BASE_PORT=$5

[[ "$BINARY" == /* ]] || BINARY="$ROOT/$BINARY"
[[ -x "$BINARY" ]] || { echo "$NAME: missing executable: $BINARY" >&2; exit 1; }
[[ -x "$RESPONDER" ]] || { echo "$NAME: build bin/r_test_responder first" >&2; exit 1; }
case "$PROFILE" in
  latency|loop|parser) ;;
  *) echo "$NAME: unknown output profile: $PROFILE" >&2; exit 2 ;;
esac
if ((BASE_PORT < 1024 || BASE_PORT > 65532)); then
  echo "$NAME: base port must be 1024..65532" >&2
  exit 2
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/r-example-e2e.XXXXXX")"
RESPONDER_PID=""
EXAMPLE_PID=""
OWNER_BASH_SUBSHELL=${BASH_SUBSHELL:-0}

cleanup() {
  # EXIT traps propagate into command substitutions and background subshells.
  # Only the owning shell may stop processes or remove diagnostic files.
  [[ "${BASH_SUBSHELL:-0}" -eq "$OWNER_BASH_SUBSHELL" ]] || return
  local pid
  for pid in "$EXAMPLE_PID" "$RESPONDER_PID"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

fail_case() {
  local scenario=$1
  local message=$2
  echo "$NAME/$scenario: $message" >&2
  for file in example.out example.err responder.out responder.err; do
    if [[ -s "$TMP_DIR/$scenario/$file" ]]; then
      echo "--- $file" >&2
      sed -n '1,100p' "$TMP_DIR/$scenario/$file" >&2
    fi
  done
  exit 1
}

wait_with_deadline() {
  local pid=$1
  local marker=$2
  local seconds=$3
  local attempts=$((seconds * 100))
  local attempt=0
  while kill -0 "$pid" 2>/dev/null && ((attempt < attempts)); do
    sleep 0.01
    attempt=$((attempt + 1))
  done
  if kill -0 "$pid" 2>/dev/null; then
    : >"$marker"
    kill -TERM "$pid" 2>/dev/null || true
    sleep 1
    kill -KILL "$pid" 2>/dev/null || true
  fi
  local status=0
  wait "$pid" || status=$?
  [[ ! -e "$marker" ]] || return 124
  return "$status"
}

wait_for_responder() {
  local scenario=$1
  local output="$TMP_DIR/$scenario/responder.out"
  for _ in {1..200}; do
    grep -q '"event":"ready"' "$output" && return 0
    kill -0 "$RESPONDER_PID" 2>/dev/null \
      || fail_case "$scenario" "responder exited before readiness"
    sleep 0.01
  done
  fail_case "$scenario" "responder readiness timed out"
}

count_events() {
  local event=$1
  local file=$2
  grep -c "\"event\":\"$event\"" "$file" 2>/dev/null || true
}

assert_example_output() {
  local scenario=$1
  local expected_status=$2
  local actual_status=$3
  local output="$TMP_DIR/$scenario/example.out"

  [[ "$actual_status" -eq "$expected_status" ]] \
    || fail_case "$scenario" \
      "example exited $actual_status; expected $expected_status"

  case "$PROFILE:$scenario" in
    loop:guard-pass)
      grep -Eq '^allowed: .+; latency=[0-9]+ ms$' "$output" \
        || fail_case "$scenario" "missing allowed outcome"
      ;;
    loop:deny)
      grep -Fxq 'denied: resource rate limit' "$output" \
        || fail_case "$scenario" "missing resource denial"
      ;;
    loop:guard-deny)
      grep -Fxq 'denied: latency guard' "$output" \
        || fail_case "$scenario" "missing latency denial"
      ;;
    latency:guard-pass)
      grep -Fxq 'guard passed: resource and latency checks admitted the work' \
        "$output" || fail_case "$scenario" "protected work was not admitted"
      grep -Eq '^latency reported: service=example-inventory-backend observed=[0-9]+ ms$' \
        "$output" || fail_case "$scenario" "latency report was not confirmed"
      ;;
    latency:deny)
      grep -Fxq 'denied: resource rate limit rejected the work' "$output" \
        || fail_case "$scenario" "missing resource denial"
      ;;
    latency:guard-deny)
      grep -Eq '^guard failed: current=[0-9]+ ms threshold=[0-9]+ ms$' "$output" \
        || fail_case "$scenario" "missing latency denial"
      ;;
    parser:guard-pass)
      grep -Eq '^protected work: GET /limited' "$output" \
        || fail_case "$scenario" "parser did not run protected work"
      grep -Fxq 'decision: allowed' "$output" \
        || fail_case "$scenario" "parser did not allow request"
      grep -Eq '^reported latency: [0-9]+ ms$' "$output" \
        || fail_case "$scenario" "parser did not report latency"
      ;;
    parser:deny)
      grep -Fxq 'decision: resource rate limited' "$output" \
        || fail_case "$scenario" "parser missed resource denial"
      ;;
    parser:guard-deny)
      grep -Fxq 'decision: latency limited' "$output" \
        || fail_case "$scenario" "parser missed latency denial"
      ;;
  esac

  if [[ "$scenario" != "guard-pass" ]] \
      && grep -Eq '^(allowed:|guard passed:|latency reported:|protected work:|reported latency:)' \
        "$output"; then
    fail_case "$scenario" "denied path executed or reported protected work"
  fi
}

assert_responder_output() {
  local scenario=$1
  local output="$TMP_DIR/$scenario/responder.out"
  local expected_reports=0
  [[ "$scenario" == "guard-pass" ]] && expected_reports=1

  [[ "$(count_events rate_request "$output")" -eq 1 ]] \
    || fail_case "$scenario" "expected exactly one rate request"
  [[ "$(count_events latency_report "$output")" -eq "$expected_reports" ]] \
    || fail_case "$scenario" \
      "expected $expected_reports latency report(s)"
  grep -Fq '"guards":1,"resources":1' "$output" \
    || fail_case "$scenario" "rate request omitted resource or latency guard"
  grep -Fq "\"label\":\"$METRICS_LABEL\"" "$output" \
    || fail_case "$scenario" "rate request used wrong metrics label"
  grep -Fq "$TRACKER_JSON" "$output" \
    || fail_case "$scenario" "tracker configuration changed"
  grep -Fq '"guard_threshold_ms":100' "$output" \
    || fail_case "$scenario" "latency threshold changed"
  grep -Fq "\"disposition\":\"$scenario\"" "$output" \
    || fail_case "$scenario" "responder observed wrong scenario"

  if [[ "$scenario" == "guard-pass" ]]; then
    grep -Eq '"observed_latency_ms":[0-9]+' "$output" \
      || fail_case "$scenario" "latency observation missing"
    grep -Fq '"matches_previous_guard":true' "$output" \
      || fail_case "$scenario" "report targeted a different tracker"
  fi
}

run_scenario() {
  local scenario=$1
  local offset=$2
  local expected_status=$3
  local port=$((BASE_PORT + offset))
  mkdir -p "$TMP_DIR/$scenario"

  "$RESPONDER" \
    "--listen=127.0.0.1:$port" \
    "--scenario=$scenario" \
    --auth=aes \
    >"$TMP_DIR/$scenario/responder.out" \
    2>"$TMP_DIR/$scenario/responder.err" &
  RESPONDER_PID=$!
  wait_for_responder "$scenario"

  (
    cd "$(dirname "$BINARY")"
    unset RATELIMITLY_TENANT
    exec env \
      RATELIMITLY_AUTH_KEY="$TEST_AES_KEY" \
      RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
      RATELIMITLY_EXAMPLE_SERVER_PORT="$port" \
      RATELIMITLY_EXAMPLE_WORK_MS=2 \
      "./$(basename "$BINARY")"
  ) >"$TMP_DIR/$scenario/example.out" \
    2>"$TMP_DIR/$scenario/example.err" &
  EXAMPLE_PID=$!
  local example_status=0
  wait_with_deadline \
    "$EXAMPLE_PID" "$TMP_DIR/$scenario/example.timeout" 10 \
    || example_status=$?
  EXAMPLE_PID=""
  [[ "$example_status" -ne 124 ]] \
    || fail_case "$scenario" "example timed out"

  # Keep receiving briefly after process exit. This catches forbidden reports
  # on denied paths and duplicate reports after a successful path.
  sleep 0.1
  kill -0 "$RESPONDER_PID" 2>/dev/null \
    || fail_case "$scenario" "responder exited before packet drain"
  kill -TERM "$RESPONDER_PID"
  local responder_status=0
  wait_with_deadline \
    "$RESPONDER_PID" "$TMP_DIR/$scenario/responder.timeout" 5 \
    || responder_status=$?
  RESPONDER_PID=""
  [[ "$responder_status" -eq 0 ]] \
    || fail_case "$scenario" "responder exited $responder_status"

  assert_example_output "$scenario" "$expected_status" "$example_status"
  assert_responder_output "$scenario"
}

run_scenario guard-pass 0 0
if [[ "$PROFILE" == "loop" ]]; then
  run_scenario deny 1 2
  run_scenario guard-deny 2 2
else
  run_scenario deny 1 0
  run_scenario guard-deny 2 0
fi

echo "$NAME: PASS (allow, resource deny, latency deny)"
