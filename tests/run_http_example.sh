#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESPONDER="$ROOT/bin/r_test_responder"
TEST_AES_KEY="rl-aes1qypsqqqqqqqqqqqrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqdgrrulcvcn0x5"
TRACKER_JSON='"tracker":{"ttl_ms":10000,"max_samples":100,"min_sample_threshold":5}'

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
  shift
  # Any further arguments are NAME=value settings for this scenario only.
  local -a extra_env=("$@")
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
        ${extra_env[@]+"${extra_env[@]}"} \
        "$KORE_EXECUTABLE" -fnrc kore.conf
    fi
    exec setsid env \
      RATELIMITLY_AUTH_KEY="$TEST_AES_KEY" \
      RATELIMITLY_EXAMPLE_SERVER_HOST=127.0.0.1 \
      RATELIMITLY_EXAMPLE_SERVER_PORT="$UDP_PORT" \
      ${extra_env[@]+"${extra_env[@]}"} \
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
  local rate_count
  local rate_line
  [[ "$scenario" == "guard-pass" ]] && expected_reports=1

  rate_count="$(count_events rate_request "$output")"
  ((rate_count >= 1 && rate_count <= 4)) \
    || fail_case "$scenario" \
      "expected one initial rate request and at most three replays; observed $rate_count"
  [[ "$(count_events latency_report "$output")" -eq "$expected_reports" ]] \
    || fail_case "$scenario" \
      "expected $expected_reports latency report(s)"
  [[ "$(count_events input_rejected "$output")" -eq 0 ]] \
    || fail_case "$scenario" "responder rejected an input packet"
  while IFS= read -r rate_line; do
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
  done < <(grep '"event":"rate_request"' "$output")

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

# CPU clock ticks used so far by every process in a session (the example and
# any workers it forked).
session_cpu_ticks() {
  local session=$1
  local total=0
  local stat_file line
  local -a fields
  for stat_file in /proc/[0-9]*/stat; do
    line=""
    { read -r line <"$stat_file"; } 2>/dev/null || continue   # the process may be gone
    # Fields after "pid (comm) ": 4 = session, 12 = utime, 13 = stime.
    read -r -a fields <<<"${line##*) }"
    if [[ "${fields[3]:-}" == "$session" ]]; then
      total=$((total + fields[11] + fields[12]))
    fi
  done
  echo "$total"
}

request_limited() {
  local scenario=$1
  local request=$2
  local body="$TMP_DIR/$scenario/response-$request.body"
  local http_status=""
  local curl_status=0
  http_status="$(curl --silent --show-error \
    --noproxy '*' \
    --header 'Connection: close' \
    --max-time 10 \
    --output "$body" \
    --write-out '%{http_code}' \
    "http://127.0.0.1:$HTTP_PORT/limited")" \
    || curl_status=$?
  cp "$body" "$TMP_DIR/$scenario/response.body" 2>/dev/null || true
  [[ "$curl_status" -eq 0 ]] \
    || fail_case "$scenario" \
      "request $request failed with curl status $curl_status"
  [[ "$http_status" == "200" ]] \
    || fail_case "$scenario" \
      "request $request: HTTP status was $http_status; expected 200"
  grep -Eq '^allowed($|[[:space:]]|\()' "$body" \
    || fail_case "$scenario" \
      "request $request: allowed response omitted protected-work result"
}

# CPU milliseconds the example's session uses during a 0.5 s sleep. Some
# frameworks run several polling workers, so idle use is measured, not assumed.
session_cpu_ms_during_sleep() {
  local clock_ticks ticks_before ticks_after
  clock_ticks="$(getconf CLK_TCK)"
  ticks_before="$(session_cpu_ticks "$SERVER_PGID")"
  sleep 0.5
  ticks_after="$(session_cpu_ticks "$SERVER_PGID")"
  echo $(((ticks_after - ticks_before) * 1000 / clock_ticks))
}

# After a request, late replies land within a few hundred milliseconds (the
# responder answers each copy 60 ms late, one after another); after that an idle
# example neither logs nor uses more CPU than it did before the first request.
assert_quiet_after() {
  local scenario=$1
  local request=$2
  local baseline_cpu_ms=$3
  local log="$TMP_DIR/$scenario/server.err"
  sleep 0.5
  local log_before log_after cpu_ms
  log_before="$(stat -c %s "$log")"
  cpu_ms="$(session_cpu_ms_during_sleep)"
  log_after="$(stat -c %s "$log")"
  local log_growth=$((log_after - log_before))
  if ((log_after > 65536)); then
    # Keep the failure report short: the first lines show the repeated error.
    head -c 16384 "$log" >"$log.head"
    mv "$log.head" "$log"
    fail_case "$scenario" \
      "after request $request, server.err reached $log_after bytes (stale UDP socket?)"
  fi
  ((log_growth <= 1024)) \
    || fail_case "$scenario" \
      "after request $request, server.err grew by $log_growth bytes in 0.5 s while idle (stale UDP socket?)"
  ((cpu_ms - baseline_cpu_ms <= 250)) \
    || fail_case "$scenario" \
      "after request $request, the example used $cpu_ms ms of CPU in 0.5 s while idle, against $baseline_cpu_ms ms before the first request (stale UDP socket?)"
}

# Source-port steering, as a kernel-UDP server sends it: every reply asks the
# client to move to new source ports, and replies arrive 60 ms late, so the
# answers to replays land after the client has already rebound. The example
# must not spin on the old sockets, and must serve the next request on the
# replacement sockets.
run_steering_scenario() {
  local scenario=steering-rebind
  UDP_PORT=$((UDP_BASE_PORT + 3))
  mkdir -p "$TMP_DIR/$scenario"

  "$RESPONDER" \
    "--listen=127.0.0.1:$UDP_PORT" \
    --scenario=guard-pass \
    --auth=aes \
    --allow-count=2 \
    --steering=rebind \
    --delay-ms=60 \
    >"$TMP_DIR/$scenario/responder.out" \
    2>"$TMP_DIR/$scenario/responder.err" &
  RESPONDER_PID=$!
  wait_for_responder "$scenario"

  # The CI request profile: 25 ms units and three replays.
  start_server "$scenario" \
    RATELIMITLY_REQUEST_UNIT_MS=25 \
    RATELIMITLY_REQUEST_REPLAY_COUNT=3
  wait_for_http "$scenario"
  local baseline_cpu_ms
  baseline_cpu_ms="$(session_cpu_ms_during_sleep)"

  local request
  for request in 1 2; do
    request_limited "$scenario" "$request"
    kill -0 "$SERVER_PID" 2>/dev/null \
      || fail_case "$scenario" "server exited after request $request"
    assert_quiet_after "$scenario" "$request" "$baseline_cpu_ms"
  done

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

  local output="$TMP_DIR/$scenario/responder.out"
  local rate_count
  rate_count="$(count_events rate_request "$output")"
  ((rate_count >= 2 && rate_count <= 8)) \
    || fail_case "$scenario" \
      "expected two rate requests with at most three replays each; observed $rate_count"
  [[ "$(count_events latency_report "$output")" -eq 2 ]] \
    || fail_case "$scenario" "expected 2 latency reports"
  [[ "$(count_events input_rejected "$output")" -eq 0 ]] \
    || fail_case "$scenario" "responder rejected an input packet"
}

# Examples that cannot run the steering case yet, each with a tracked issue.
steering_skip_reason() {
  case "$NAME" in
    lwan)
      echo "lwan crashes on replies slower than about 60 ms:" \
        "https://github.com/ratelimitly-com/rl-c-client/issues/75"
      ;;
  esac
}

run_scenario guard-pass 0
run_scenario deny 1
run_scenario guard-deny 2

steering_skip="$(steering_skip_reason)"
if [[ -n "$steering_skip" ]]; then
  echo "$NAME: SKIP source-port steering ($steering_skip)"
  echo "$NAME: PASS (HTTP 200, resource deny, latency deny)"
else
  run_steering_scenario
  echo "$NAME: PASS (HTTP 200, resource deny, latency deny, source-port steering)"
fi
