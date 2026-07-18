#!/usr/bin/env bash
set -euo pipefail

AUTH_KEY=""
AUTH_KEY_INPUT_ERROR=""
case "${RATELIMITLY_AUTH_KEY_FD:-}" in
  3)
    IFS= read -r AUTH_KEY <&3 \
      || AUTH_KEY_INPUT_ERROR="could not read authentication key descriptor"
    exec 3<&-
    ;;
  "")
    # Direct invocation remains convenient, but the inherited value is removed
    # before this script runs any external setup command.
    AUTH_KEY=${RATELIMITLY_AUTH_KEY:-}
    ;;
  *)
    AUTH_KEY_INPUT_ERROR="RATELIMITLY_AUTH_KEY_FD must be 3"
    ;;
esac
unset RATELIMITLY_AUTH_KEY
unset RATELIMITLY_AUTH_KEY_FD
export -n AUTH_KEY
exec </dev/null

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

die() {
  echo "run_production_p0_http_example: $*" >&2
  exit 1
}

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "run_production_p0_http_example: SKIP (Linux only)"
  exit 0
fi
if [[ "$#" -ne 4 ]]; then
  echo "usage: $0 <name> <binary-or-module> <http-port> <launcher>" >&2
  exit 2
fi

NAME=$1
ARTIFACT=$2
HTTP_PORT=$3
LAUNCHER=$4

[[ "$NAME" =~ ^[A-Za-z0-9_.-]+$ ]] || die "invalid example name"
[[ "$HTTP_PORT" =~ ^[0-9]+$ ]] || die "$NAME: HTTP port is not numeric"
((HTTP_PORT >= 1024 && HTTP_PORT <= 65535)) \
  || die "$NAME: HTTP port must be 1024..65535"
[[ "$LAUNCHER" == "direct" || "$LAUNCHER" == "kore" ]] \
  || die "$NAME: unknown launcher: $LAUNCHER"

[[ -z "$AUTH_KEY_INPUT_ERROR" ]] || die "$NAME: $AUTH_KEY_INPUT_ERROR"
[[ -n "$AUTH_KEY" ]] \
  || die "$NAME: RATELIMITLY_AUTH_KEY is required"
ulimit -c 0 || die "$NAME: could not disable core dumps"
for variable in \
  RATELIMITLY_TENANT \
  RATELIMITLY_EXAMPLE_SERVER_HOST \
  RATELIMITLY_EXAMPLE_SERVER_PORT; do
  [[ -z "${!variable+x}" ]] \
    || die "$NAME: $variable must be unset for production discovery"
done
# Defense in depth: no discovery override is inherited by the framework.
unset RATELIMITLY_TENANT
unset RATELIMITLY_EXAMPLE_SERVER_HOST
unset RATELIMITLY_EXAMPLE_SERVER_PORT

[[ "$ARTIFACT" == /* ]] || ARTIFACT="$ROOT/$ARTIFACT"
[[ -e "$ARTIFACT" ]] || die "$NAME: missing artifact: $ARTIFACT"
case "$LAUNCHER" in
  direct)
    [[ -x "$ARTIFACT" ]] || die "$NAME: artifact is not executable"
    ;;
  kore)
    KORE_EXECUTABLE="${KORE_EXECUTABLE:-${KORE_ROOT:-}/kore}"
    [[ -x "$KORE_EXECUTABLE" ]] \
      || die "$NAME: set KORE_ROOT or KORE_EXECUTABLE"
    ;;
esac

for command in curl mktemp ps setsid sleep; do
  command -v "$command" >/dev/null 2>&1 \
    || die "$NAME: required command is unavailable: $command"
done

umask 077
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/r-production-http.XXXXXX")
SERVER_PID=""
SERVER_PGID=""
OWNER_BASHPID=$BASHPID

group_has_live_processes() {
  local target_group=$1
  local listing=""
  local process_group
  local process_state

  [[ -n "$target_group" ]] || return 1
  if ! listing=$(ps -eo pgid=,stat= 2>/dev/null); then
    # If process inspection fails, assume the group still needs termination.
    return 0
  fi
  while read -r process_group process_state; do
    if [[ "$process_group" == "$target_group" \
        && "$process_state" != Z* ]]; then
      return 0
    fi
  done <<<"$listing"
  return 1
}

direct_pid_is_reapable() {
  local pid=$1
  local process_state=""

  if process_state=$(ps -o stat= -p "$pid" 2>/dev/null); then
    [[ "$process_state" =~ ^[[:space:]]*Z ]]
    return
  fi
  ! kill -0 "$pid" 2>/dev/null
}

wait_for_group_exit() {
  local process_group=$1
  local attempts=$2
  local delay=$3
  local attempt

  for ((attempt = 0; attempt < attempts; attempt++)); do
    group_has_live_processes "$process_group" || return 0
    sleep "$delay"
  done
  ! group_has_live_processes "$process_group"
}

stop_server() {
  local pid=$SERVER_PID
  local process_group=$SERVER_PGID
  local result=0
  local reaped=0
  local attempt

  [[ -n "$pid" ]] || return 0

  # The examples may own workers. Signal the isolated session as a group, give
  # it a bounded grace period, then make cleanup unconditional with SIGKILL.
  # Signal the direct child too: before setsid completes, the expected group
  # may not exist yet, while the unreaped child PID cannot be reused.
  [[ -z "$process_group" ]] \
    || kill -TERM -- "-$process_group" 2>/dev/null || true
  kill -TERM "$pid" 2>/dev/null || true
  if [[ -n "$process_group" ]] \
      && ! wait_for_group_exit "$process_group" 10 0.05; then
    kill -KILL -- "-$process_group" 2>/dev/null || true
    wait_for_group_exit "$process_group" 20 0.02 || result=1
  fi
  if ! direct_pid_is_reapable "$pid"; then
    kill -KILL "$pid" 2>/dev/null || true
  fi

  # Never call wait(1) on a live child. Poll until it is absent or a zombie so
  # the final reap itself cannot extend beyond the outer timeout's kill window.
  for ((attempt = 0; attempt < 50; attempt++)); do
    if direct_pid_is_reapable "$pid"; then
      wait "$pid" 2>/dev/null || true
      reaped=1
      break
    fi
    sleep 0.02
  done
  ((reaped == 1)) || result=1
  if [[ -n "$process_group" ]] \
      && group_has_live_processes "$process_group"; then
    result=1
  fi

  SERVER_PID=""
  SERVER_PGID=""
  return "$result"
}

cleanup() {
  # EXIT traps can be inherited by subshells. Only the fixture owner cleans.
  [[ "$BASHPID" -eq "$OWNER_BASHPID" ]] || return
  stop_server || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

sanitize_stream() {
  local line
  local key_token
  local lines=0

  while IFS= read -r line || [[ -n "$line" ]]; do
    lines=$((lines + 1))
    ((lines <= 120)) || break
    line=${line//"$AUTH_KEY"/[REDACTED_AUTH_KEY]}
    while [[ "$line" =~ rl-(aes|hmac)[[:alnum:]_-]+ ]]; do
      key_token=${BASH_REMATCH[0]}
      line=${line//"$key_token"/[REDACTED_AUTH_KEY]}
    done
    printf '%s\n' "$line"
  done
}

fail_case() {
  local message=$1
  local file
  echo "$NAME: $message" >&2
  for file in ready.body ready.err response.body request.err server.out server.err; do
    if [[ -s "$TMP_DIR/$file" ]]; then
      echo "--- $file (sanitized)" >&2
      sanitize_stream <"$TMP_DIR/$file" >&2
    fi
  done
  exit 1
}

start_server() {
  local directory
  directory=$(dirname "$ARTIFACT")

  # Export the captured credential only in the framework launch subshell. It is
  # never an argv element and no setup, curl, ps, or logging process inherits it.
  (
    cd "$directory"
    export RATELIMITLY_AUTH_KEY="$AUTH_KEY"
    if [[ "$LAUNCHER" == "kore" ]]; then
      exec setsid "$KORE_EXECUTABLE" -fnrc kore.conf
    fi
    exec setsid "$ARTIFACT"
  ) >"$TMP_DIR/server.out" 2>"$TMP_DIR/server.err" &
  SERVER_PID=$!
  SERVER_PGID=$SERVER_PID

  # Verify setsid(1) established the group we will later terminate. This avoids
  # claiming isolation while accidentally leaving framework workers behind.
  local actual_pgid=""
  for _ in {1..100}; do
    if actual_pgid=$(ps -o pgid= -p "$SERVER_PID" 2>/dev/null); then
      actual_pgid=${actual_pgid//[[:space:]]/}
    else
      actual_pgid=""
    fi
    [[ "$actual_pgid" == "$SERVER_PGID" ]] && return
    kill -0 "$SERVER_PID" 2>/dev/null \
      || fail_case "server exited before creating its process group"
    sleep 0.01
  done
  fail_case "server did not enter an isolated process group"
}

assert_http_port_is_free() {
  # Bash's /dev/tcp reports any accepting TCP listener, including a non-HTTP
  # process that curl-based probing could mistake for a merely slow endpoint.
  if (exec 9<>"/dev/tcp/127.0.0.1/$HTTP_PORT") 2>/dev/null; then
    fail_case "HTTP port $HTTP_PORT is already occupied"
  fi
}

wait_for_http() {
  local deadline=$((SECONDS + 20))
  local status=""
  while ((SECONDS < deadline)); do
    if status=$(curl \
        --silent \
        --show-error \
        --noproxy '*' \
        --proto '=http' \
        --header 'Connection: close' \
        --connect-timeout 0.25 \
        --max-time 0.5 \
        --output "$TMP_DIR/ready.body" \
        --write-out '%{http_code}' \
        "http://127.0.0.1:$HTTP_PORT/__ratelimitly_ready" \
        2>"$TMP_DIR/ready.err"); then
      [[ "$status" =~ ^[1-5][0-9][0-9]$ ]] && return
    fi
    kill -0 "$SERVER_PID" 2>/dev/null \
      || fail_case "server exited before HTTP readiness"
    sleep 0.05
  done
  fail_case "HTTP readiness timed out after 20 seconds"
}

request_limited_resource() {
  local status=""
  local curl_status=0
  local telemetry_log
  if status=$(curl \
      --silent \
      --show-error \
      --noproxy '*' \
      --proto '=http' \
      --header 'Connection: close' \
      --connect-timeout 1 \
      --max-time 15 \
      --output "$TMP_DIR/response.body" \
      --write-out '%{http_code}' \
      "http://127.0.0.1:$HTTP_PORT/limited" \
      2>"$TMP_DIR/request.err"); then
    :
  else
    curl_status=$?
    fail_case "GET /limited failed with curl status $curl_status"
  fi

  [[ "$status" == "200" ]] \
    || fail_case "GET /limited returned HTTP $status; expected 200"
  [[ -s "$TMP_DIR/response.body" ]] \
    || fail_case "GET /limited returned an empty body"
  kill -0 "$SERVER_PID" 2>/dev/null \
    || fail_case "server exited after GET /limited"
  # Some framework callbacks queue the HTTP response before run_and_report()
  # returns. Keep the process alive for one short, bounded drain so a telemetry
  # failure logged immediately after curl receives the body cannot race PASS.
  sleep 0.1
  for telemetry_log in server.out server.err; do
    if grep -Fq 'latency report failed' "$TMP_DIR/$telemetry_log" \
        2>/dev/null; then
      fail_case "framework logged a latency-report failure"
    fi
  done
  if grep -Eiq \
      'denied|rate-limit (service unavailable|check failed|task failed)' \
      "$TMP_DIR/response.body"; then
    fail_case "HTTP 200 carried a denied or unavailable response body"
  fi
  # Every example emits this marker only after admitted protected work ran and
  # the latency sample was handed to the client for reporting.
  grep -Eq '^allowed($|[[:space:]]|\()' "$TMP_DIR/response.body" \
    || fail_case "HTTP 200 body omitted the allowed protected-work marker"
  kill -0 "$SERVER_PID" 2>/dev/null \
    || fail_case "server exited after GET /limited"
}

assert_http_port_is_free
start_server
wait_for_http
request_limited_resource
stop_server || fail_case "server cleanup exceeded its bounded deadline"

echo "$NAME: PASS (production P0 rate admission and latency-report path)"
