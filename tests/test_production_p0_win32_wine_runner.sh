#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNNER="$ROOT/tests/test_production_p0_win32_wine.sh"
TEST_NAME=test_production_p0_win32_wine_runner

fail() {
  echo "$TEST_NAME: $*" >&2
  exit 1
}

if [[ $(uname -s) != Linux ]]; then
  echo "$TEST_NAME: SKIP (Linux process-group semantics required)"
  exit 0
fi
for command in setsid timeout; do
  command -v "$command" >/dev/null 2>&1 \
    || fail "required command is unavailable: $command"
done

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/r-wine-runner-test.XXXXXX")
FIXTURE_BIN=$TMP_ROOT/bin
RUNNER_PID=""
mkdir -p "$FIXTURE_BIN"

cleanup() {
  local child_pid_file child_pid
  if [[ -n $RUNNER_PID ]] && kill -0 "$RUNNER_PID" 2>/dev/null; then
    kill -TERM "$RUNNER_PID" 2>/dev/null || true
    /bin/sleep 0.1
    kill -KILL "$RUNNER_PID" 2>/dev/null || true
    wait "$RUNNER_PID" 2>/dev/null || true
  fi
  while IFS= read -r child_pid_file; do
    child_pid=$(<"$child_pid_file")
    kill -KILL -- "-$child_pid" 2>/dev/null || true
    kill -KILL "$child_pid" 2>/dev/null || true
  done < <(find "$TMP_ROOT" -name child.pid -type f 2>/dev/null)
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

# The sleep shim skips only the production tracker's 11-second stale-state
# drain. Every short process-cleanup wait still uses the system implementation.
cat >"$FIXTURE_BIN/sleep" <<'FIXTURE'
#!/usr/bin/env bash
if [[ ${1:-} == 11 ]]; then
  exit 0
fi
exec /bin/sleep "$@"
FIXTURE

cat >"$FIXTURE_BIN/fake-wine" <<'FIXTURE'
#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} == cmd ]]; then
  printf 'key=%s\n' "${RATELIMITLY_AUTH_KEY:+present}" \
    >>"$FIXTURE_STATE_DIR/wine-init.log"
  exit 0
fi

printf '%s\n' "$BASHPID" >"$FIXTURE_STATE_DIR/child.pid"
printf 'key=%s\n' "${RATELIMITLY_AUTH_KEY:+present}" \
  >>"$FIXTURE_STATE_DIR/example.log"
case ${FAKE_WINE_MODE:-success} in
  success)
    printf '%s\r\n' \
      'allowed: inventory response prepared by Win32; latency=7 ms'
    ;;
  bad-output)
    printf '%s\n' 'unexpected output'
    printf 'credential leak fixture: %s\n' "$RATELIMITLY_AUTH_KEY" >&2
    ;;
  hang)
    # Keep cleanup in its graceful phase long enough for the test to deliver
    # a second cancellation signal. The runner must ignore that signal while
    # it completes the bounded KILL fallback.
    trap '' TERM
    exec /bin/sleep 30
    ;;
  *)
    exit 64
    ;;
esac
FIXTURE

cat >"$FIXTURE_BIN/fake-wineserver" <<'FIXTURE'
#!/usr/bin/env bash
set -euo pipefail
printf 'key=%s args=%s\n' "${RATELIMITLY_AUTH_KEY:+present}" "$*" \
  >>"$FIXTURE_STATE_DIR/wineserver.log"
case ${FAKE_SERVER_MODE:-success} in
  success)
    exit 0
    ;;
  kill-missing)
    [[ ${1:-} != -k ]]
    ;;
  fail)
    exit 1
    ;;
  *)
    exit 64
    ;;
esac
FIXTURE

printf '%s\n' fixture >"$FIXTURE_BIN/win32-example.exe"
chmod +x "$FIXTURE_BIN/sleep" "$FIXTURE_BIN/fake-wine" \
  "$FIXTURE_BIN/fake-wineserver"

AUTH_KEY=rl-aes-fixture-secret-1234567890

new_case() {
  local name=$1
  local directory=$TMP_ROOT/$name
  mkdir -p "$directory/tmp"
  printf '%s\n' "$directory"
}

run_fixture() {
  local directory=$1
  local wine_mode=$2
  local server_mode=$3
  env \
    PATH="$FIXTURE_BIN:$PATH" \
    TMPDIR="$directory/tmp" \
    RATELIMITLY_AUTH_KEY="$AUTH_KEY" \
    WINE_SERVER="$FIXTURE_BIN/fake-wineserver" \
    FIXTURE_STATE_DIR="$directory" \
    FAKE_WINE_MODE="$wine_mode" \
    FAKE_SERVER_MODE="$server_mode" \
    bash "$RUNNER" \
      "$FIXTURE_BIN/win32-example.exe" \
      "$FIXTURE_BIN/fake-wine"
}

assert_prefix_removed() {
  local directory=$1
  if compgen -G "$directory/tmp/r-production-p0-wine.*" >/dev/null; then
    fail "isolated Wine prefix survived cleanup"
  fi
}

pid_is_live() {
  local pid=$1
  local state
  state=$(ps -o stat= -p "$pid" 2>/dev/null || true)
  state=${state//[[:space:]]/}
  [[ -n $state && $state != Z* ]]
}

test_success() {
  local directory
  directory=$(new_case success)
  run_fixture "$directory" success success \
    >"$directory/runner.out" 2>"$directory/runner.err" \
    || fail "valid output did not pass"

  grep -Fxq -- \
    'test_production_p0_win32_wine: PASS (production P0 admission and latency report)' \
    "$directory/runner.out" \
    || fail "success did not report the documented assurance"
  [[ ! -s $directory/runner.err ]] \
    || fail "success wrote unexpected stderr"
  grep -Fxq 'key=' "$directory/wine-init.log" \
    || fail "Wine prefix initialization inherited the credential"
  grep -Fxq 'key=present' "$directory/example.log" \
    || fail "the Win32 child did not inherit the credential"
  [[ $(grep -Fxc 'key= args=-k' "$directory/wineserver.log") -eq 2 ]] \
    || fail "success did not issue both prefix-scoped wineserver kills"
  [[ $(grep -Fxc 'key= args=-w' "$directory/wineserver.log") -eq 2 ]] \
    || fail "success did not wait for both wineserver shutdowns"
  assert_prefix_removed "$directory"
}

test_redaction() {
  local directory status=0
  directory=$(new_case redaction)
  run_fixture "$directory" bad-output success \
    >"$directory/runner.out" 2>"$directory/runner.err" || status=$?

  [[ $status -ne 0 ]] || fail "invalid output unexpectedly passed"
  if grep -Fq "$AUTH_KEY" "$directory/runner.out" "$directory/runner.err"; then
    fail "failure diagnostics exposed the inherited credential"
  fi
  grep -Fq '[REDACTED_AUTH_KEY]' "$directory/runner.err" \
    || fail "failure diagnostics did not prove credential redaction"
  assert_prefix_removed "$directory"
}

test_wineserver_failure() {
  local directory status=0
  directory=$(new_case wineserver-failure)
  run_fixture "$directory" success fail \
    >"$directory/runner.out" 2>"$directory/runner.err" || status=$?

  [[ $status -ne 0 ]] || fail "wineserver shutdown failure unexpectedly passed"
  grep -Fq 'isolated wineserver did not stop cleanly' "$directory/runner.err" \
    || fail "wineserver shutdown failure was not reported"
  grep -Fxq 'key= args=-k' "$directory/wineserver.log" \
    || fail "failed cleanup did not attempt wineserver -k"
  grep -Fxq 'key= args=-w' "$directory/wineserver.log" \
    || fail "failed cleanup did not attempt wineserver -w"
  assert_prefix_removed "$directory"
}

test_already_stopped_wineserver() {
  local directory
  directory=$(new_case already-stopped-server)
  run_fixture "$directory" success kill-missing \
    >"$directory/runner.out" 2>"$directory/runner.err" \
    || fail "an already-stopped wineserver caused a false failure"

  grep -Fxq -- \
    'test_production_p0_win32_wine: PASS (production P0 admission and latency report)' \
    "$directory/runner.out" \
    || fail "already-stopped wineserver did not preserve success"
  [[ ! -s $directory/runner.err ]] \
    || fail "already-stopped wineserver wrote unexpected stderr"
  assert_prefix_removed "$directory"
}

test_signal_cleanup() {
  local directory child_pid status=0
  directory=$(new_case signal-cleanup)
  env \
    PATH="$FIXTURE_BIN:$PATH" \
    TMPDIR="$directory/tmp" \
    RATELIMITLY_AUTH_KEY="$AUTH_KEY" \
    WINE_SERVER="$FIXTURE_BIN/fake-wineserver" \
    FIXTURE_STATE_DIR="$directory" \
    FAKE_WINE_MODE=hang \
    FAKE_SERVER_MODE=success \
    bash "$RUNNER" \
      "$FIXTURE_BIN/win32-example.exe" \
      "$FIXTURE_BIN/fake-wine" \
    >"$directory/runner.out" 2>"$directory/runner.err" &
  RUNNER_PID=$!

  for _ in {1..200}; do
    [[ -s $directory/child.pid ]] && break
    kill -0 "$RUNNER_PID" 2>/dev/null \
      || fail "runner exited before the hanging child started"
    /bin/sleep 0.01
  done
  [[ -s $directory/child.pid ]] || fail "hanging child did not start"
  child_pid=$(<"$directory/child.pid")

  kill -TERM "$RUNNER_PID"
  /bin/sleep 0.1
  kill -TERM "$RUNNER_PID" \
    || fail "runner exited before bounded cancellation cleanup completed"
  wait "$RUNNER_PID" || status=$?
  RUNNER_PID=""
  [[ $status -eq 143 ]] \
    || fail "TERM cleanup exited $status; expected 143"
  for _ in {1..40}; do
    pid_is_live "$child_pid" || break
    /bin/sleep 0.05
  done
  if pid_is_live "$child_pid"; then
    ps -o pid=,ppid=,pgid=,sid=,stat=,comm= -p "$child_pid" >&2 || true
    fail "TERM cleanup left the isolated Wine child alive"
  fi
  assert_prefix_removed "$directory"
}

test_success
test_redaction
test_wineserver_failure
test_already_stopped_wineserver
test_signal_cleanup

echo "$TEST_NAME: PASS"
