#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESPONDER="$ROOT/bin/r_test_responder"
TEST_AES_KEY="rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l"
TRACKER_JSON='"tracker":{"ttl_ms":10000,"max_samples":100,"buffer_size":32,"min_sample_threshold":5}'

if [[ "$#" -ne 7 ]]; then
  echo "usage: $0 <name> <binary-or-module> <http-port> <metrics-label> <resource-deny-status> <launcher> <udp-base-port>" >&2
  exit 2
fi

NAME=$1
ARTIFACT=$2
HTTP_PORT=$3
METRICS_LABEL=$4
RESOURCE_DENY_STATUS=$5
LAUNCHER=$6
UDP_BASE_PORT=$7

[[ "$ARTIFACT" == /* ]] || ARTIFACT="$ROOT/$ARTIFACT"
[[ -e "$ARTIFACT" ]] || { echo "$NAME: missing artifact: $ARTIFACT" >&2; exit 1; }
[[ -x "$RESPONDER" ]] || { echo "$NAME: build bin/r_test_responder first" >&2; exit 1; }
case "$LAUNCHER" in
  direct)
    [[ -x "$ARTIFACT" ]] \
      || { echo "$NAME: example is not executable: $ARTIFACT" >&2; exit 1; }
    ;;
  kore)
    KORE_EXECUTABLE="${KORE_EXECUTABLE:-${KORE_ROOT:-}/kore}"
    [[ -x "$KORE_EXECUTABLE" ]] \
      || { echo "$NAME: set KORE_ROOT or KORE_EXECUTABLE" >&2; exit 1; }
    ;;
  *)
    echo "$NAME: unknown launcher: $LAUNCHER" >&2
    exit 2
    ;;
esac
if ((HTTP_PORT < 1024 || HTTP_PORT > 65535)); then
  echo "$NAME: HTTP port must be 1024..65535" >&2
  exit 2
fi
if ((UDP_BASE_PORT < 1024 || UDP_BASE_PORT > 65532)); then
  echo "$NAME: UDP base port must be 1024..65532" >&2
  exit 2
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/r-http-example-e2e.XXXXXX")"
RESPONDER_PID=""
SERVER_PID=""
SERVER_PGID=""
UDP_PORT=""
OWNER_BASHPID=$BASHPID

stop_server() {
  if [[ -z "$SERVER_PGID" ]]; then
    return
  fi

  # Some frameworks own worker processes. Signal the session created by
  # setsid(1), not only its leader, so no listener survives into the next case.
  kill -TERM -- "-$SERVER_PGID" 2>/dev/null || true
  for _ in {1..100}; do
    [[ -z "$SERVER_PID" ]] && break
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.01
  done
  kill -KILL -- "-$SERVER_PGID" 2>/dev/null || true
  if [[ -n "$SERVER_PID" ]]; then
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  SERVER_PID=""
  SERVER_PGID=""
}

cleanup() {
  # EXIT traps are inherited by command substitutions and background shells.
  # Only the shell which created the fixtures may tear them down.
  [[ "$BASHPID" -eq "$OWNER_BASHPID" ]] || return
  stop_server
  if [[ -n "$RESPONDER_PID" ]] && kill -0 "$RESPONDER_PID" 2>/dev/null; then
    kill -TERM "$RESPONDER_PID" 2>/dev/null || true
    wait_with_deadline "$RESPONDER_PID" 2 >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

fail_case() {
  local scenario=$1
  local message=$2
  echo "$NAME/$scenario: $message" >&2
  local file
  for file in response.body ready.body server.out server.err responder.out responder.err; do
    if [[ -s "$TMP_DIR/$scenario/$file" ]]; then
      echo "--- $file" >&2
      sed -n '1,120p' "$TMP_DIR/$scenario/$file" >&2
    fi
  done
  exit 1
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
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    return 124
  fi
  wait "$pid"
}

start_server() {
  local scenario=$1
  local directory
  directory="$(dirname "$ARTIFACT")"

  # The key is synthetic. The explicit host/port routes only this test process
  # to the local responder; production examples still default to P0 discovery.
  (
    cd "$directory"
    unset RATELIMITLY_TENANT
    if [[ "$LAUNCHER" == "kore" ]]; then
      exec setsid env \
        RATELIMITLY_AUTH_KEY="$TEST_AES_KEY" \
        RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
        RATELIMITLY_EXAMPLE_SERVER_PORT="$UDP_PORT" \
        "$KORE_EXECUTABLE" -fnrc kore.conf
    fi
    exec setsid env \
      RATELIMITLY_AUTH_KEY="$TEST_AES_KEY" \
      RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
      RATELIMITLY_EXAMPLE_SERVER_PORT="$UDP_PORT" \
      "./$(basename "$ARTIFACT")"
  ) >"$TMP_DIR/$scenario/server.out" \
    2>"$TMP_DIR/$scenario/server.err" &
  SERVER_PID=$!
  SERVER_PGID=$SERVER_PID
}

wait_for_http() {
  local scenario=$1
  local status
  local curl_status
  for _ in {1..300}; do
    status=""
    curl_status=0
    status="$(curl --silent --show-error \
      --noproxy '*' \
      --header 'Connection: close' \
      --max-time 0.5 \
      --output "$TMP_DIR/$scenario/ready.body" \
      --write-out '%{http_code}' \
      "http://127.0.0.1:$HTTP_PORT/__ratelimitly_ready" \
      2>/dev/null)" \
      || curl_status=$?
    if [[ "$curl_status" -eq 0 && "$status" =~ ^[1-5][0-9][0-9]$ ]]; then
      return 0
    fi
    kill -0 "$SERVER_PID" 2>/dev/null \
      || fail_case "$scenario" "server exited before HTTP readiness"
    sleep 0.02
  done
  fail_case "$scenario" "HTTP readiness timed out"
}

count_events() {
  local event=$1
  local file=$2
  grep -c "\"event\":\"$event\"" "$file" 2>/dev/null || true
}

assert_http_result() {
  local scenario=$1
  local actual_status=$2
  local expected_status=503
  local body="$TMP_DIR/$scenario/response.body"
  case "$scenario" in
    guard-pass) expected_status=200 ;;
    deny) expected_status=$RESOURCE_DENY_STATUS ;;
    guard-deny) expected_status=503 ;;
  esac

  [[ "$actual_status" == "$expected_status" ]] \
    || fail_case "$scenario" \
      "HTTP status was $actual_status; expected $expected_status"
  [[ -s "$body" ]] || fail_case "$scenario" "HTTP response body was empty"
  if [[ "$scenario" == "guard-pass" ]]; then
    grep -Eq '^allowed($|[[:space:]]|\()' "$body" \
      || fail_case "$scenario" "allowed response omitted protected-work result"
  elif grep -Fqi 'allowed' "$body"; then
    fail_case "$scenario" "denied response exposed an allowed result"
  fi
}

assert_responder_output() {
  local scenario=$1
  local output="$TMP_DIR/$scenario/responder.out"
  local expected_reports=0
  local rate_line
  [[ "$scenario" == "guard-pass" ]] && expected_reports=1

  [[ "$(count_events rate_request "$output")" -eq 1 ]] \
    || fail_case "$scenario" "expected exactly one rate request"
  [[ "$(count_events latency_report "$output")" -eq "$expected_reports" ]] \
    || fail_case "$scenario" \
      "expected $expected_reports latency report(s)"
  [[ "$(count_events input_rejected "$output")" -eq 0 ]] \
    || fail_case "$scenario" "responder rejected an input packet"
  rate_line="$(grep '"event":"rate_request"' "$output")"
  grep -Fq '"guards":1,"resources":1' <<<"$rate_line" \
    || fail_case "$scenario" "request omitted resource or latency admission"
  grep -Fq "\"label\":\"$METRICS_LABEL\"" <<<"$rate_line" \
    || fail_case "$scenario" "request used wrong metrics label"
  grep -Fq "$TRACKER_JSON" <<<"$rate_line" \
    || fail_case "$scenario" "tracker configuration changed"
  grep -Fq '"guard_threshold_ms":100' <<<"$rate_line" \
    || fail_case "$scenario" "latency threshold changed"
  grep -Fq "\"disposition\":\"$scenario\"" <<<"$rate_line" \
    || fail_case "$scenario" "responder observed wrong scenario"

  if [[ "$scenario" == "guard-pass" ]]; then
    local latency_line
    latency_line="$(grep '"event":"latency_report"' "$output")"
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
  local offset=$2
  UDP_PORT=$((UDP_BASE_PORT + offset))
  mkdir -p "$TMP_DIR/$scenario"

  "$RESPONDER" \
    "--listen=127.0.0.1:$UDP_PORT" \
    "--scenario=$scenario" \
    --auth=aes \
    >"$TMP_DIR/$scenario/responder.out" \
    2>"$TMP_DIR/$scenario/responder.err" &
  RESPONDER_PID=$!
  wait_for_responder "$scenario"

  start_server "$scenario"
  wait_for_http "$scenario"

  local http_status=""
  local curl_status=0
  http_status="$(curl --silent --show-error \
    --noproxy '*' \
    --header 'Connection: close' \
    --max-time 10 \
    --output "$TMP_DIR/$scenario/response.body" \
    --write-out '%{http_code}' \
    "http://127.0.0.1:$HTTP_PORT/limited")" \
    || curl_status=$?
  [[ "$curl_status" -eq 0 ]] \
    || fail_case "$scenario" "HTTP request failed with curl status $curl_status"
  kill -0 "$SERVER_PID" 2>/dev/null \
    || fail_case "$scenario" "server exited after serving the request"

  # Drain while the server is alive, then once more after shutdown. This makes
  # duplicate or forbidden late reports visible before fixture assertions.
  sleep 0.1
  stop_server
  sleep 0.1

  kill -0 "$RESPONDER_PID" 2>/dev/null \
    || fail_case "$scenario" "responder exited before packet drain"
  kill -TERM "$RESPONDER_PID"
  local responder_status=0
  wait_with_deadline "$RESPONDER_PID" 5 || responder_status=$?
  RESPONDER_PID=""
  [[ "$responder_status" -eq 0 ]] \
    || fail_case "$scenario" "responder exited $responder_status"

  assert_http_result "$scenario" "$http_status"
  assert_responder_output "$scenario"
}

run_scenario guard-pass 0
run_scenario deny 1
run_scenario guard-deny 2

echo "$NAME: PASS (HTTP 200, resource deny, latency deny)"
