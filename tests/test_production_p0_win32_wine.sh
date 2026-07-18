#!/usr/bin/env bash
set -euo pipefail

TEST_NAME=test_production_p0_win32_wine

usage() {
  echo "usage: $0 [win32-example.exe [wine-runner]]" >&2
  exit 2
}

fail() {
  echo "$TEST_NAME: $*" >&2
  exit 1
}

[[ $# -le 2 ]] || usage

# Paths may be positional for CI or supplied by an existing cross-build job.
# The production credential itself is accepted only as inherited environment.
EXAMPLE_PATH=${1:-${WINDOWS_EXAMPLE_BINARY:-}}
WINE_RUNNER=${2:-${WINDOWS_RUNNER:-}}
[[ -n $EXAMPLE_PATH && -f $EXAMPLE_PATH && -r $EXAMPLE_PATH ]] \
  || fail "the Windows example path is missing or unreadable"

[[ -n ${RATELIMITLY_AUTH_KEY:-} ]] \
  || fail "RATELIMITLY_AUTH_KEY is required"
AUTH_KEY=$RATELIMITLY_AUTH_KEY
readonly AUTH_KEY
unset RATELIMITLY_AUTH_KEY
ulimit -c 0 2>/dev/null \
  || fail "core dumps could not be disabled before loading the CI credential"

# Production P0 discovery must come only from the key. Empty inherited values
# are removed as well, so the PE executable cannot observe an override.
for variable in \
  RATELIMITLY_TENANT \
  RATELIMITLY_EXAMPLE_SERVER_HOST \
  RATELIMITLY_EXAMPLE_SERVER_PORT; do
  [[ -z ${!variable:-} ]] \
    || fail "$variable must not override key-derived production discovery"
  unset "$variable"
done

if [[ -z $WINE_RUNNER ]]; then
  for candidate in wine64 wine /usr/lib/wine/wine64; do
    if command -v "$candidate" >/dev/null 2>&1; then
      WINE_RUNNER=$(command -v "$candidate")
      break
    fi
  done
elif [[ $WINE_RUNNER != */* ]]; then
  WINE_RUNNER=$(command -v "$WINE_RUNNER" || true)
fi
[[ -n $WINE_RUNNER && -x $WINE_RUNNER ]] \
  || fail "a Wine runner is required"

WINE_SERVER=${WINE_SERVER:-}
if [[ -z $WINE_SERVER ]]; then
  for candidate in \
    wineserver \
    /usr/lib/wine/wineserver64 \
    /usr/lib/wine/wineserver; do
    if command -v "$candidate" >/dev/null 2>&1; then
      WINE_SERVER=$(command -v "$candidate")
      break
    fi
  done
elif [[ $WINE_SERVER != */* ]]; then
  WINE_SERVER=$(command -v "$WINE_SERVER" || true)
fi
[[ -n $WINE_SERVER && -x $WINE_SERVER ]] \
  || fail "wineserver is required for bounded prefix cleanup"

for command in awk mktemp ps setsid sleep timeout; do
  command -v "$command" >/dev/null 2>&1 \
    || fail "required command is unavailable: $command"
done

umask 077
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/r-production-p0-wine.XXXXXX")
WINE_PREFIX=$TMP_DIR/wine-prefix
mkdir -m 700 "$WINE_PREFIX"
WINEPREFIX=$WINE_PREFIX
WINEDEBUG=-all
WINEARCH=win64
export WINEPREFIX WINEDEBUG WINEARCH
PROCESS_PID=""
PROCESS_PGID=""
WINE_STARTED=false
LAUNCH_IN_PROGRESS=false
PENDING_SIGNAL=0

group_has_live_processes() {
  [[ -n $PROCESS_PGID ]] || return 1
  ps -eo pgid=,stat= | awk -v group="$PROCESS_PGID" '
    $1 == group && $2 !~ /^Z/ { found = 1 }
    END { exit found ? 0 : 1 }
  '
}

process_is_live() {
  [[ -n $PROCESS_PID ]] || return 1
  local state
  state=$(ps -o stat= -p "$PROCESS_PID" 2>/dev/null || true)
  state=${state//[[:space:]]/}
  [[ -n $state && $state != Z* ]]
}

clear_process_tracking() {
  PROCESS_PID=""
  PROCESS_PGID=""
}

wait_for_tree_exit() {
  local attempts=$1
  local pid=$PROCESS_PID
  local attempt

  for ((attempt = 0; attempt < attempts; attempt++)); do
    if ! process_is_live && ! group_has_live_processes; then
      # A dead or zombie child makes wait nonblocking and reaps its status.
      [[ -z $pid ]] || wait "$pid" 2>/dev/null || true
      clear_process_tracking
      return 0
    fi
    sleep 0.05
  done
  return 1
}

kill_process_tree() {
  [[ -n $PROCESS_PID || -n $PROCESS_PGID ]] || return 0

  [[ -z $PROCESS_PGID ]] \
    || kill -KILL -- "-$PROCESS_PGID" 2>/dev/null || true
  [[ -z $PROCESS_PID ]] || kill -KILL "$PROCESS_PID" 2>/dev/null || true
  wait_for_tree_exit 40
}

stop_process_tree() {
  [[ -n $PROCESS_PID || -n $PROCESS_PGID ]] || return 0

  # Every Wine invocation is a new session. Terminate that entire process
  # group, allow two seconds of grace, then use the bounded hard-kill path.
  [[ -z $PROCESS_PGID ]] \
    || kill -TERM -- "-$PROCESS_PGID" 2>/dev/null || true
  [[ -z $PROCESS_PID ]] || kill -TERM "$PROCESS_PID" 2>/dev/null || true
  if wait_for_tree_exit 40; then
    return 0
  fi
  kill_process_tree
}

stop_wineserver() {
  $WINE_STARTED || return 0
  local status=0
  (
    set +e
    unset RATELIMITLY_AUTH_KEY
    unset RATELIMITLY_TENANT
    unset RATELIMITLY_EXAMPLE_SERVER_HOST
    unset RATELIMITLY_EXAMPLE_SERVER_PORT
    # -k may legitimately report that the server already exited. The bounded
    # -w result is authoritative: success proves the prefix server is gone.
    timeout --signal=KILL 5s "$WINE_SERVER" -k || true
    timeout --signal=KILL 5s "$WINE_SERVER" -w
  ) >/dev/null 2>&1 || status=$?
  if ((status == 0)); then
    WINE_STARTED=false
  fi
  return "$status"
}

cleanup() {
  local status=$?
  local cleanup_failed=false
  trap - EXIT
  trap '' HUP INT TERM
  set +e

  stop_process_tree || cleanup_failed=true
  stop_wineserver || cleanup_failed=true
  if $cleanup_failed; then
    # wineserver -k is prefix-scoped and may release a Wine process that did
    # not respond to Unix signals. Check the isolated tree once more.
    kill_process_tree || true
    stop_wineserver || true
    if process_is_live || group_has_live_processes; then
      echo "$TEST_NAME: isolated Wine process tree survived cleanup" >&2
      status=1
    fi
    if $WINE_STARTED; then
      echo "$TEST_NAME: isolated wineserver did not stop cleanly" >&2
      status=1
    fi
  fi
  rm -rf "$TMP_DIR" || status=1
  exit "$status"
}
trap cleanup EXIT

handle_signal() {
  local status=$1
  if $LAUNCH_IN_PROGRESS; then
    PENDING_SIGNAL=$status
    return
  fi
  exit "$status"
}

finish_launch() {
  LAUNCH_IN_PROGRESS=false
  if ((PENDING_SIGNAL != 0)); then
    exit "$PENDING_SIGNAL"
  fi
}

trap 'handle_signal 129' HUP
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM

sanitize_stream() {
  # The exact environment value is replaced literally. The second rule also
  # catches parsed or reconstructed credential-shaped text from dependencies.
  # Sanitization stays in Bash so no diagnostic helper inherits the secret.
  local line protected token
  local count=0
  while IFS= read -r line || [[ -n $line ]]; do
    if ((count == 80)); then
      echo "[diagnostic output truncated after 80 lines]"
      break
    fi
    protected=${line//"$AUTH_KEY"/[REDACTED_AUTH_KEY]}
    while [[ $protected =~ (rl-(aes|hmac)[[:alnum:]_-]+) ]]; do
      token=${BASH_REMATCH[1]}
      protected=${protected//"$token"/[REDACTED_AUTH_KEY]}
    done
    if ((${#protected} > 1000)); then
      protected="${protected:0:1000} [line truncated]"
    fi
    printf '%s\n' "$protected"
    count=$((count + 1))
  done
}

fail_case() {
  local message=$1
  local file
  echo "$TEST_NAME: $message" >&2
  for file in example.out example.err wine-init.out wine-init.err; do
    if [[ -s $TMP_DIR/$file ]]; then
      echo "--- $file (sanitized)" >&2
      sanitize_stream <"$TMP_DIR/$file" >&2
    fi
  done
  exit 1
}

wait_with_deadline() {
  local seconds=$1
  local deadline=$((SECONDS + seconds))
  local status=0

  while process_is_live || group_has_live_processes; do
    if ((SECONDS >= deadline)); then
      # The 60-second production deadline is hard: do not add a graceful
      # interval after it expires. Kill the isolated process tree immediately.
      kill_process_tree || return 125
      return 124
    fi
    sleep 0.05
  done
  wait "$PROCESS_PID" || status=$?
  clear_process_tracking
  return "$status"
}

initialize_prefix() {
  # Warm the brand-new prefix without exposing the production key to Wine's
  # setup helpers. This keeps setup notices out of the example's stderr while
  # retaining a fresh, isolated prefix for every test run.
  WINE_STARTED=true
  LAUNCH_IN_PROGRESS=true
  (
    unset RATELIMITLY_AUTH_KEY
    unset RATELIMITLY_TENANT
    unset RATELIMITLY_EXAMPLE_SERVER_HOST
    unset RATELIMITLY_EXAMPLE_SERVER_PORT
    exec setsid "$WINE_RUNNER" cmd /c exit 0
  ) >"$TMP_DIR/wine-init.out" 2>"$TMP_DIR/wine-init.err" &
  PROCESS_PID=$!
  PROCESS_PGID=$PROCESS_PID
  finish_launch

  local status=0
  wait_with_deadline 30 || status=$?
  [[ $status -ne 124 ]] \
    || fail_case "Wine prefix initialization exceeded 30 seconds"
  [[ $status -ne 125 ]] \
    || fail_case "Wine prefix initialization could not be terminated"
  [[ $status -eq 0 ]] \
    || fail_case "Wine prefix initialization exited $status"
  stop_wineserver
}

run_example() {
  # WINEPREFIX and WINEDEBUG are non-secret execution controls. The key is
  # inherited directly, never repeated in env(1) arguments or process argv.
  WINE_STARTED=true
  LAUNCH_IN_PROGRESS=true
  (
    export RATELIMITLY_AUTH_KEY=$AUTH_KEY
    exec setsid "$WINE_RUNNER" "$EXAMPLE_PATH"
  ) >"$TMP_DIR/example.out" 2>"$TMP_DIR/example.err" &
  PROCESS_PID=$!
  PROCESS_PGID=$PROCESS_PID
  finish_launch

  local status=0
  wait_with_deadline 60 || status=$?
  [[ $status -ne 124 ]] \
    || fail_case "example exceeded the 60-second hard deadline"
  [[ $status -ne 125 ]] \
    || fail_case "example exceeded its deadline and survived cleanup"
  [[ $status -eq 0 ]] \
    || fail_case "example exited $status; expected 0"
}

assert_output() {
  # awk sees one logical line for LF or CRLF input. A second line (including a
  # blank line), extra text, or a different message makes the assertion fail.
  awk '
    {
      sub(/\r$/, "")
      if (NR == 1 &&
          $0 ~ /^allowed: inventory response prepared by Win32; latency=[0-9]+ ms$/) {
        valid = 1
      } else {
        valid = 0
      }
    }
    END { exit !(NR == 1 && valid) }
  ' "$TMP_DIR/example.out" \
    || fail_case "stdout was not the single documented allowed/latency line"
  [[ ! -s $TMP_DIR/example.err ]] \
    || fail_case "example wrote unexpected stderr"
}

initialize_prefix
# The example's tracker retains samples for ten seconds. CI serializes the
# fixed Win32 identities; this drain prevents a cancelled prior run from
# influencing the fresh production admission below.
echo "$TEST_NAME: draining stale production state (11 seconds)"
sleep 11
run_example
assert_output
stop_wineserver

echo "$TEST_NAME: PASS (production P0 admission and latency report)"
