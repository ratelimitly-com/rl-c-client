#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/examples/manifest.txt"
README="${R_EXAMPLE_README_PATH:-$ROOT/examples/README.md}"
ROOT_README="$ROOT/README.md"
API_GUIDE="$ROOT/docs/api.md"
IO_GUIDE="$ROOT/IO_ABSTRACTION.md"
CLOUD_CI_GUIDE="$ROOT/docs/cloud-server-ci-plan.md"
CI_WORKFLOW="${R_CI_WORKFLOW_PATH:-$ROOT/.github/workflows/ci.yml}"
H2O_MAKEFILE="$ROOT/examples/h2o/Makefile"
H2O_CMAKE="$ROOT/examples/h2o/CMakeLists.txt"
LWAN_MAKEFILE="$ROOT/examples/lwan/Makefile"
LWAN_CMAKE="$ROOT/examples/lwan/CMakeLists.txt"
GLIB_SOURCE="$ROOT/examples/glib/main.c"
KORE_SOURCE="$ROOT/examples/kore/main.c"
PERF_SOURCE="$ROOT/bin/perf_client.c"
LINUX_ONE_SHOT_MATRIX="$ROOT/tests/linux-one-shot-examples.txt"
LINUX_HTTP_MATRIX="$ROOT/tests/linux-http-examples.txt"
MACOS_LOCAL_MATRIX="$ROOT/tests/macos-local-examples.txt"
WINDOWS_NATIVE_RUNNER="$ROOT/tests/test_windows_native_example.ps1"
PRODUCTION_P0_SOURCE="$ROOT/tests/production_p0_probe.c"
PRODUCTION_P0_RUNNER="$ROOT/tests/test_production_p0.sh"
PRODUCTION_P0_ONE_SHOT_RUNNER="$ROOT/tests/test_production_p0_one_shot_examples.sh"
PRODUCTION_P0_HTTP_MATRIX_RUNNER="$ROOT/tests/test_production_p0_http_examples.sh"
PRODUCTION_P0_HTTP_RUNNER="$ROOT/tests/run_production_p0_http_example.sh"
PRODUCTION_P0_WIN32_WINE_RUNNER="$ROOT/tests/test_production_p0_win32_wine.sh"
PRODUCTION_P0_WIN32_WINE_TEST="$ROOT/tests/test_production_p0_win32_wine_runner.sh"
PRODUCTION_P0_WIN32_NATIVE_RUNNER="$ROOT/tests/test_production_p0_win32_example.ps1"
EXPECTED_PRODUCTION_GATE="github.ref=='refs/heads/main'&&(github.event_name=='push'||(github.event_name=='workflow_dispatch'&&github.actor=='edescourtis'))"
PRODUCTION_SECRET_BINDING='RATELIMITLY_AUTH_KEY: ${{ secrets.RATELIMITLY_AUTH_KEY }}'

fail() {
  echo "test_examples: $*" >&2
  exit 1
}

require_main_only_concurrency() {
  local job_name=$1
  local job_block=$2
  local concurrency_block
  concurrency_block=$(sed -n '/^    concurrency:/,/^    steps:/p' \
    <<<"$job_block")
  grep -Fq -- "github.ref == 'refs/heads/main'" <<<"$concurrency_block" \
    || fail "$job_name does not reserve production concurrency for main"
  if grep -Fq -- "github.event_name == 'workflow_dispatch'" \
      <<<"$concurrency_block"; then
    fail "$job_name routes feature dispatches through production concurrency"
  fi
}

count_fixed_occurrences() {
  local needle=$1
  local text=$2
  awk -v needle="$needle" '
    index($0, needle) { count++ }
    END { print count + 0 }
  ' <<<"$text"
}

require_production_job_gate() {
  local job_name=$1
  local step_name=$2
  local expected_command=$3
  local job_block=$4
  local expression step_block step_count command_count secret_count
  expression=$(awk '
    $0 == "    if: >-" { capture = 1; next }
    capture && /^    [A-Za-z_][A-Za-z_-]*:/ { exit }
    capture { gsub(/[[:space:]]/, ""); printf "%s", $0 }
  ' <<<"$job_block")
  [[ "$expression" == "$EXPECTED_PRODUCTION_GATE" ]] \
    || fail "$job_name does not use the exact trusted-main production gate"
  step_count=$(awk -v target="      - name: $step_name" '
    $0 == target { count++ }
    END { print count + 0 }
  ' <<<"$job_block")
  [[ "$step_count" -eq 1 ]] \
    || fail "$job_name must contain one production step: $step_name"
  step_block=$(awk -v target="      - name: $step_name" '
    $0 == target { capture = 1 }
    capture && $0 != target && /^      - / { exit }
    capture { print }
  ' <<<"$job_block")
  command_count=$(count_fixed_occurrences "$expected_command" "$job_block")
  secret_count=$(count_fixed_occurrences \
    "$PRODUCTION_SECRET_BINDING" "$job_block")
  [[ "$command_count" -eq 1 ]] \
    || fail "$job_name must contain exactly one production command"
  [[ "$secret_count" -eq 1 ]] \
    || fail "$job_name must contain exactly one production secret binding"
  grep -Fq -- "$PRODUCTION_SECRET_BINDING" <<<"$step_block" \
    || fail "$job_name production command does not receive the repository secret"
  grep -Fq -- "$expected_command" <<<"$step_block" \
    || fail "$job_name gate is not bound to its production command"
}

require_production_step_gate() {
  local job_name=$1
  local step_name=$2
  local expected_command=$3
  local job_block=$4
  local step_block expression step_count command_count secret_count
  step_count=$(awk -v target="      - name: $step_name" '
    $0 == target { count++ }
    END { print count + 0 }
  ' <<<"$job_block")
  [[ "$step_count" -eq 1 ]] \
    || fail "$job_name must contain one production step: $step_name"
  step_block=$(awk -v target="      - name: $step_name" '
    $0 == target { capture = 1 }
    capture && $0 != target && /^      - / { exit }
    capture { print }
  ' <<<"$job_block")
  expression=$(awk '
    $0 == "        if: >-" { capture = 1; next }
    capture && /^        [A-Za-z_][A-Za-z_-]*:/ { exit }
    capture { gsub(/[[:space:]]/, ""); printf "%s", $0 }
  ' <<<"$step_block")
  [[ "$expression" == "$EXPECTED_PRODUCTION_GATE" ]] \
    || fail "$job_name does not use the exact trusted-main production gate"
  command_count=$(count_fixed_occurrences "$expected_command" "$job_block")
  secret_count=$(count_fixed_occurrences \
    "$PRODUCTION_SECRET_BINDING" "$job_block")
  [[ "$command_count" -eq 1 ]] \
    || fail "$job_name must contain exactly one production command"
  [[ "$secret_count" -eq 1 ]] \
    || fail "$job_name must contain exactly one production secret binding"
  grep -Fq -- "$PRODUCTION_SECRET_BINDING" <<<"$step_block" \
    || fail "$job_name production command does not receive the repository secret"
  grep -Fq -- "$expected_command" <<<"$step_block" \
    || fail "$job_name gate is not bound to its production command"
}

[[ -f "$MANIFEST" ]] || fail "missing examples/manifest.txt"
[[ -f "$LINUX_ONE_SHOT_MATRIX" ]] \
  || fail "missing executable Linux one-shot example matrix"
[[ -f "$LINUX_HTTP_MATRIX" ]] \
  || fail "missing executable Linux HTTP example matrix"
[[ -f "$MACOS_LOCAL_MATRIX" ]] \
  || fail "missing local-only macOS example matrix"
[[ -f "$WINDOWS_NATIVE_RUNNER" ]] \
  || fail "missing native Windows behavioral runner"
[[ -f "$PRODUCTION_P0_SOURCE" ]] \
  || fail "missing production P0 protocol probe"
[[ -x "$PRODUCTION_P0_RUNNER" ]] \
  || fail "missing executable production P0 runner"
[[ -x "$PRODUCTION_P0_ONE_SHOT_RUNNER" ]] \
  || fail "missing executable production P0 one-shot runner"
[[ -x "$PRODUCTION_P0_HTTP_MATRIX_RUNNER" ]] \
  || fail "missing executable production P0 HTTP matrix runner"
[[ -x "$PRODUCTION_P0_HTTP_RUNNER" ]] \
  || fail "missing executable production P0 HTTP example runner"
[[ -x "$PRODUCTION_P0_WIN32_WINE_RUNNER" ]] \
  || fail "missing executable Wine production P0 runner"
[[ -x "$PRODUCTION_P0_WIN32_WINE_TEST" ]] \
  || fail "missing executable Wine P0 runner behavioral test"
[[ -f "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" ]] \
  || fail "missing native Win32 production P0 runner"
[[ -f "$CLOUD_CI_GUIDE" ]] \
  || fail "missing production P0 CI guide"
for required_text in \
  'Status: implemented' \
  'repository secret' \
  'key-derived tenant' \
  'trusted main' \
  'synthetic-only' \
  '11 one-shot' \
  '10 HTTP' \
  'native MSVC' \
  'MinGW/Wine' \
  '37 ms' \
  'positive token deficit' \
  'does not prove that production accepted' \
  'kqueue and libdispatch remain local-only' \
  'server binary' \
  'fire-and-forget' \
  'layered'; do
  grep -Fiq -- "$required_text" "$CLOUD_CI_GUIDE" \
    || fail "production P0 CI guide omits: $required_text"
done
grep -Fq -- '```mermaid' "$CLOUD_CI_GUIDE" \
  || fail "production P0 CI guide has no architecture diagram"
for stale_text in \
  'lease broker' \
  'RATELIMITLY_EXAMPLE_SERVER_ID' \
  'short-lived tenant'; do
  if grep -Fiq -- "$stale_text" "$CLOUD_CI_GUIDE"; then
    fail "production P0 CI guide retains stale design: $stale_text"
  fi
done
if grep -Eq -- \
    'c-[0-9]{8,}|s-[0-9]{6,}|rl-(aes|hmac)[A-Za-z0-9_-]{32,}' \
    "$CLOUD_CI_GUIDE"; then
  fail "production P0 CI guide contains a concrete credential or endpoint"
fi
grep -Fxq -- '/bin/production_p0_probe' "$ROOT/.gitignore" \
  || fail "production P0 build artifact is not ignored"
grep -Fq -- 'bash tests/test_linux_one_shot_examples.sh' "$CI_WORKFLOW" \
  || fail "CI does not execute Linux one-shot examples"
grep -Fq -- 'bash tests/test_linux_http_examples.sh' "$CI_WORKFLOW" \
  || fail "CI does not execute Linux HTTP examples"
grep -Fq -- '## CI validation layers' "$README" \
  || fail "examples README does not explain layered CI validation"
grep -Fq -- '../docs/cloud-server-ci-plan.md' "$README" \
  || fail "examples README does not link the production P0 CI guide"
[[ $(grep -Fc -- '```mermaid' "$README") -ge 2 ]] \
  || fail "examples README has no CI validation diagram"
expected_linux_one_shot=$'latency_tracker\nlibuv\nlibevent\nglib\nlibev\nsd_event\nlibhv\nliburing\nepoll\nio_uring\nllhttp'
actual_linux_one_shot="$(sed -n '/^[^#]/s/|.*//p' "$LINUX_ONE_SHOT_MATRIX")"
[[ "$actual_linux_one_shot" == "$expected_linux_one_shot" ]] \
  || fail "Linux one-shot matrix is incomplete or out of order"
expected_linux_http=$'mongoose|a\ncivetweb|a\nlibmicrohttpd|a\nulfius|a\nh2o|b\nlwan|b\nlibreactor|b\nfacil_io|c\nonion|c\nkore|c'
actual_linux_http="$(sed -n '/^[^#]/s/^\([^|]*|[^|]*\).*/\1/p' \
  "$LINUX_HTTP_MATRIX")"
[[ "$actual_linux_http" == "$expected_linux_http" ]] \
  || fail "Linux HTTP matrix is incomplete, mis-sharded, or out of order"
expected_macos_local=$'kqueue\nlibdispatch'
actual_macos_local="$(sed -n '/^[^#]/s/|.*//p' "$MACOS_LOCAL_MATRIX")"
[[ "$actual_macos_local" == "$expected_macos_local" ]] \
  || fail "local macOS matrix is incomplete or out of order"
if grep -Eq -- 'test_macos_examples|macos-local-examples' "$CI_WORKFLOW"; then
  fail "CI must not execute the local-only macOS example matrix"
fi
if grep -Fq -- 'BASHPID' "$ROOT/tests/run_one_shot_example.sh"; then
  fail "local macOS runner depends on Bash 4 BASHPID"
fi
for scenario in guard-pass deny guard-deny; do
  grep -Fq -- "run_scenario $scenario" \
    "$ROOT/tests/run_one_shot_example.sh" \
    || fail "Linux one-shot runner omits $scenario"
  grep -Fq -- "run_scenario $scenario" \
    "$ROOT/tests/run_http_example.sh" \
    || fail "Linux HTTP runner omits $scenario"
done
grep -Fq -- "'\"reports\":1'" "$ROOT/tests/run_http_example.sh" \
  || fail "Linux HTTP runner does not require exactly one reported sample"
grep -Fq -- 'macos-latest' "$CI_WORKFLOW" \
  || fail "CI does not validate the macOS build"
grep -Fq -- 'tests/test_windows_example.sh' "$CI_WORKFLOW" \
  || fail "CI does not build and run the Win32 example"
grep -Fq -- 'tests/test_windows_responder.sh' "$CI_WORKFLOW" \
  || fail "Wine CI does not compile the Windows responder"
grep -Fq -- "'\"event\":\"latency_report\"'" \
  "$ROOT/tests/test_windows_responder.sh" \
  || fail "Windows responder test does not run an authenticated PE roundtrip"
grep -Fq -- 'add_executable(r-test-responder' \
  "$ROOT/examples/win32/CMakeLists.txt" \
  || fail "native Windows CMake does not build the test responder"
for scenario in guard-pass deny guard-deny; do
  grep -Fq -- "run_scenario $scenario" "$ROOT/tests/test_windows_example.sh" \
    || fail "Win32 behavioral runner omits $scenario"
done
if grep -Fq -- '--max-packets' "$ROOT/tests/test_windows_example.sh"; then
  fail "Win32 responder exits before late-packet assertions"
fi
grep -Fq -- 'KORE_SYSCALL_ALLOW(connect)' "$KORE_SOURCE" \
  || fail "Kore seccomp blocks key-derived production DNS"
grep -Fq -- \
  'KORE_SYSCALL_DENY_ARG(socket, 0, AF_UNIX, ENOENT)' "$KORE_SOURCE" \
  || fail "Kore seccomp does not safely reject the glibc nscd probe"
if grep -Fq -- 'KORE_SYSCALL_ALLOW_ARG(socket, 0, AF_UNIX)' "$KORE_SOURCE"; then
  fail "Kore seccomp grants unnecessary access to local Unix sockets"
fi
grep -Fq -- \
  'KORE_SYSCALL_DENY_ARG(socket, 0, AF_NETLINK, EAFNOSUPPORT)' "$KORE_SOURCE" \
  || fail "Kore seccomp does not safely reject the optional netlink probe"
if grep -Fq -- 'KORE_SYSCALL_ALLOW_ARG(socket, 0, AF_NETLINK)' "$KORE_SOURCE"; then
  fail "Kore seccomp grants unnecessary netlink access"
fi
grep -Fq -- 'KORE_SYSCALL_ALLOW(sendmmsg)' "$KORE_SOURCE" \
  || fail "Kore seccomp blocks batched A/AAAA queries"
if grep -Fq -- 'KORE_SYSCALL_ALLOW(recvmsg)' "$KORE_SOURCE"; then
  fail "Kore seccomp grants unnecessary recvmsg access"
fi
if grep -Eq -- 'KORE_SYSCALL_ALLOW(_ARG)?\(ioctl' "$KORE_SOURCE"; then
  fail "Kore seccomp grants unnecessary ioctl access"
fi
grep -Fq -- 'runs-on: windows-latest' "$CI_WORKFLOW" \
  || fail "CI does not validate the Win32 example on native Windows"
grep -Fq -- 'tests/test_windows_native_example.ps1' "$CI_WORKFLOW" \
  || fail "native Windows CI does not run admission scenarios"
grep -Fq -- 'bash tests/test_production_p0.sh' "$CI_WORKFLOW" \
  || fail "CI does not run the key-only production P0 probe"
grep -Fq -- 'RATELIMITLY_AUTH_KEY: ${{ secrets.RATELIMITLY_AUTH_KEY }}' \
  "$CI_WORKFLOW" \
  || fail "production P0 CI does not consume the repository secret"
grep -Fq -- "github.ref == 'refs/heads/main'" "$CI_WORKFLOW" \
  || fail "production P0 CI is not restricted to main"
if grep -Fq -- "refs/heads/codex/example-integrations" "$CI_WORKFLOW"; then
  fail "production P0 CI retains a feature-branch validation exception"
fi
[[ $(grep -Fc -- "github.actor == 'edescourtis'" "$CI_WORKFLOW") -eq 5 ]] \
  || fail "every manual production P0 entry point must be owner-restricted"
[[ $(grep -Fc -- "$PRODUCTION_SECRET_BINDING" "$CI_WORKFLOW") -eq 5 ]] \
  || fail "production credentials must be bound only to five verified steps"
production_p0_job=$(sed -n \
  '/^  production-p0-smoke:/,/^  linux-one-shot-examples:/p' \
  "$CI_WORKFLOW")
require_production_job_gate \
  "production protocol CI" "Prove key-only rate and latency behavior" \
  "bash tests/test_production_p0.sh" "$production_p0_job"
grep -Fq -- 'cancel-in-progress: false' "$CI_WORKFLOW" \
  || fail "production P0 CI does not serialize shared-tenant tests"
grep -Fq -- 'unset RATELIMITLY_TENANT' "$PRODUCTION_P0_RUNNER" \
  || fail "production P0 runner does not guard key-derived discovery"
grep -Fq -- 'unset RATELIMITLY_EXAMPLE_SERVER_HOST' "$PRODUCTION_P0_RUNNER" \
  || fail "production P0 runner does not guard fixed-host bypasses"
grep -Fq -- 'unset RATELIMITLY_EXAMPLE_SERVER_PORT' "$PRODUCTION_P0_RUNNER" \
  || fail "production P0 runner does not guard fixed-port bypasses"
grep -Eq -- 'timeout [0-9]+' "$PRODUCTION_P0_RUNNER" \
  || fail "production P0 runner has no process deadline"
grep -Fq -- 'r_client_admission_report_latency(' "$PRODUCTION_P0_SOURCE" \
  || fail "production P0 probe does not report latency"
grep -Fq -- 'latency_limited' "$PRODUCTION_P0_SOURCE" \
  || fail "production P0 probe does not read latency state back"
grep -Fq -- 'rate_limited' "$PRODUCTION_P0_SOURCE" \
  || fail "production P0 probe does not prove rate limiting"
grep -Fq -- 'tokens_deficit == 0u' "$PRODUCTION_P0_SOURCE" \
  || fail "production P0 probe accepts an unproven rate denial"
grep -Fq -- 'if (second_outcome.allowed' "$PRODUCTION_P0_SOURCE" \
  || fail "production P0 probe accepts a contradictory allowed rate outcome"
linux_one_shot_job=$(sed -n \
  '/^  linux-one-shot-examples:/,/^  linux-http-examples:/p' \
  "$CI_WORKFLOW")
require_production_step_gate \
  "Linux one-shot CI" "Run examples against production P0" \
  "bash tests/test_production_p0_one_shot_examples.sh" \
  "$linux_one_shot_job"
grep -Fq -- 'bash tests/test_production_p0_one_shot_examples.sh' \
  <<<"$linux_one_shot_job" \
  || fail "Linux one-shot CI does not run examples against production P0"
grep -Fq -- 'RATELIMITLY_AUTH_KEY: ${{ secrets.RATELIMITLY_AUTH_KEY }}' \
  <<<"$linux_one_shot_job" \
  || fail "Linux one-shot production step does not use the repository secret"
grep -Fq -- "github.actor == 'edescourtis'" <<<"$linux_one_shot_job" \
  || fail "Linux one-shot manual production run is not restricted"
require_main_only_concurrency "Linux one-shot CI" "$linux_one_shot_job"
grep -Fq -- 'cancel-in-progress: false' <<<"$linux_one_shot_job" \
  || fail "Linux one-shot production runs are not serialized"
grep -Fq -- 'unset RATELIMITLY_TENANT' "$PRODUCTION_P0_ONE_SHOT_RUNNER" \
  || fail "one-shot P0 runner does not force key-derived discovery"
grep -Eq -- \
  '^[[:space:]]*timeout --signal=TERM --kill-after=1s 29s ' \
  "$PRODUCTION_P0_ONE_SHOT_RUNNER" \
  || fail "one-shot P0 runner lacks its hard 30-second deadline"
linux_http_job=$(sed -n \
  '/^  linux-http-examples:/,/^  win32-example:/p' \
  "$CI_WORKFLOW")
require_production_step_gate \
  "Linux HTTP CI" "Run HTTP frameworks against production P0" \
  "bash tests/test_production_p0_http_examples.sh" "$linux_http_job"
grep -Fq -- \
  'bash tests/test_production_p0_http_examples.sh "${{ matrix.shard }}"' \
  <<<"$linux_http_job" \
  || fail "Linux HTTP CI does not run frameworks against production P0"
grep -Fq -- 'RATELIMITLY_AUTH_KEY: ${{ secrets.RATELIMITLY_AUTH_KEY }}' \
  <<<"$linux_http_job" \
  || fail "Linux HTTP production step does not use the repository secret"
grep -Fq -- "github.actor == 'edescourtis'" <<<"$linux_http_job" \
  || fail "Linux HTTP manual production run is not restricted"
require_main_only_concurrency "Linux HTTP CI" "$linux_http_job"
grep -Fq -- 'cancel-in-progress: false' <<<"$linux_http_job" \
  || fail "Linux HTTP production runs are not serialized"
grep -Fq -- 'unset RATELIMITLY_TENANT' "$PRODUCTION_P0_HTTP_MATRIX_RUNNER" \
  || fail "HTTP P0 matrix does not force key-derived discovery"
grep -Fq -- 'sleep 11' "$PRODUCTION_P0_HTTP_MATRIX_RUNNER" \
  || fail "HTTP P0 matrix does not expire stale tracker state"
grep -Fq -- 'exec setsid' "$PRODUCTION_P0_HTTP_RUNNER" \
  || fail "HTTP P0 runner does not isolate framework process groups"
grep -Fq -- '--max-time 15' "$PRODUCTION_P0_HTTP_RUNNER" \
  || fail "HTTP P0 runner has no bounded protected request"
grep -Eq -- \
  '^[[:space:]]*exec timeout --signal=TERM --kill-after=5s 55s ' \
  "$PRODUCTION_P0_HTTP_MATRIX_RUNNER" \
  || fail "HTTP P0 matrix lacks a hard per-framework deadline"
grep -Fq -- 'kill -KILL -- "-$process_group"' "$PRODUCTION_P0_HTTP_RUNNER" \
  || fail "HTTP P0 runner cannot force-stop framework workers"
grep -Fq -- "grep -Fq 'latency report failed'" "$PRODUCTION_P0_HTTP_RUNNER" \
  || fail "HTTP P0 runner ignores local latency-report failures"
grep -Fq -- 'unset RATELIMITLY_AUTH_KEY' "$PRODUCTION_P0_HTTP_RUNNER" \
  || fail "HTTP P0 helper leaks the key to support processes"
grep -Fq -- 'ulimit -c 0' "$PRODUCTION_P0_HTTP_RUNNER" \
  || fail "HTTP P0 runner permits secret-bearing core dumps"
grep -Fq -- 'assert_http_port_is_free' "$PRODUCTION_P0_HTTP_RUNNER" \
  || fail "HTTP P0 runner can attach to an unrelated listener"
bash -n \
  "$PRODUCTION_P0_HTTP_MATRIX_RUNNER" \
  "$PRODUCTION_P0_HTTP_RUNNER" \
  || fail "HTTP P0 runner has invalid Bash syntax"
if grep -Eq -- 'c-2213169720275691601|s-408232124743711' \
    "$PRODUCTION_P0_HTTP_MATRIX_RUNNER" "$PRODUCTION_P0_HTTP_RUNNER"; then
  fail "HTTP P0 runner hard-codes one production endpoint"
fi
win32_wine_job=$(sed -n '/^  win32-example:/,/^  win32-msvc:/p' "$CI_WORKFLOW")
require_production_step_gate \
  "Wine CI" "Run Wine Win32 example against production P0" \
  "bash tests/test_production_p0_win32_wine.sh" "$win32_wine_job"
grep -Fq -- 'cmake -S examples/win32 -B build-mingw' \
  <<<"$win32_wine_job" \
  || fail "Wine CI does not create one reusable CMake cross-build"
grep -Fq -- '(LIB_EAY|OPENSSL_CRYPTO_LIBRARY(_RELEASE)?):FILEPATH=' \
  <<<"$win32_wine_job" \
  || fail "Wine CI does not accept supported CMake OpenSSL cache spellings"
grep -Fq -- 'OPENSSL_INCLUDE_DIR:PATH=' <<<"$win32_wine_job" \
  || fail "Wine CI does not verify the MinGW OpenSSL headers"
grep -Fq -- "'DLL Name: .*lib(crypto|ssl).*\\.dll'" \
  <<<"$win32_wine_job" \
  || fail "Wine CI does not reject dynamic OpenSSL imports"
grep -Fq -- \
  'WINDOWS_EXAMPLE_BINARY: ${{ github.workspace }}/build-mingw/win32-example.exe' \
  <<<"$win32_wine_job" \
  || fail "Wine deterministic tests do not reuse the CMake-built PE"
grep -Fq -- 'tests/test_production_p0_win32_wine.sh' \
  <<<"$win32_wine_job" \
  || fail "Wine CI does not run the Win32 client against production P0"
grep -Fq -- 'RATELIMITLY_AUTH_KEY: ${{ secrets.RATELIMITLY_AUTH_KEY }}' \
  <<<"$win32_wine_job" \
  || fail "Wine production step does not use the repository secret"
grep -Fq -- "github.actor == 'edescourtis'" <<<"$win32_wine_job" \
  || fail "Wine manual production run is not restricted"
require_main_only_concurrency "Wine CI" "$win32_wine_job"
grep -Fq -- 'rl-c-client-production-win32' <<<"$win32_wine_job" \
  || fail "Wine and native Windows production state is not serialized"
grep -Fq -- 'wait_with_deadline 60' "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
  || fail "Wine P0 runner lacks a 60-second hard client deadline"
grep -Fq -- 'sleep 11' "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
  || fail "Wine P0 runner does not expire stale tracker state"
grep -Fq -- 'ulimit -c 0' "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
  || fail "Wine P0 runner permits secret-bearing core dumps"
if grep -Eq -- 'ulimit -c 0.*\|\| true' \
    "$PRODUCTION_P0_WIN32_WINE_RUNNER"; then
  fail "Wine P0 runner does not fail closed when core dumps stay enabled"
fi
grep -Fq -- 'WINEARCH=win64' "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
  || fail "Wine P0 runner does not create a deterministic 64-bit prefix"
grep -Fq -- 'wait_for_tree_exit 40' "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
  || fail "Wine P0 runner does not verify hard-kill completion"
grep -Fq -- 'stop_wineserver || cleanup_failed=true' \
  "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
  || fail "Wine P0 runner ignores wineserver shutdown failures"
grep -Fq -- 'inventory response prepared by Win32' \
  "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
  || fail "Wine P0 runner does not assert protected work"
for variable in \
  RATELIMITLY_TENANT \
  RATELIMITLY_EXAMPLE_SERVER_HOST \
  RATELIMITLY_EXAMPLE_SERVER_PORT; do
  grep -Fq -- "$variable" "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
    || fail "Wine P0 runner does not clear $variable"
done
bash -n "$PRODUCTION_P0_WIN32_WINE_RUNNER" \
  || fail "Wine P0 runner has invalid Bash syntax"
if [[ $(uname -s) == Linux ]]; then
  bash "$PRODUCTION_P0_WIN32_WINE_TEST" \
    || fail "Wine P0 runner behavioral tests failed"
fi
if grep -Eq -- 'c-2213169720275691601|s-408232124743711' \
    "$PRODUCTION_P0_WIN32_WINE_RUNNER"; then
  fail "Wine P0 runner hard-codes one production endpoint"
fi
win32_msvc_job=$(sed -n '/^  win32-msvc:/,$p' "$CI_WORKFLOW")
require_production_step_gate \
  "native Win32 CI" "Run native Win32 example against production P0" \
  "tests/test_production_p0_win32_example.ps1" "$win32_msvc_job"
grep -Fq -- 'tests/test_production_p0_win32_example.ps1' \
  <<<"$win32_msvc_job" \
  || fail "native MSVC CI does not run the Win32 client against P0"
grep -Fq -- 'RATELIMITLY_AUTH_KEY: ${{ secrets.RATELIMITLY_AUTH_KEY }}' \
  <<<"$win32_msvc_job" \
  || fail "native Win32 P0 step does not use the repository secret"
grep -Fq -- "github.actor == 'edescourtis'" <<<"$win32_msvc_job" \
  || fail "native Win32 manual production run is not restricted"
require_main_only_concurrency "native Win32 CI" "$win32_msvc_job"
grep -Fq -- 'rl-c-client-production-win32' <<<"$win32_msvc_job" \
  || fail "native Win32 production state is not serialized"
grep -Fq -- 'WaitForExit(60000)' "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner lacks a 60-second deadline"
grep -Fq -- '.Kill($true)' "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner cannot kill the process tree"
grep -Fq -- '$Stopped = $Process.WaitForExit(5000)' \
  "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner does not verify tree-kill completion"
grep -Fq -- 'taskkill.exe' "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner lacks the bounded taskkill fallback"
grep -Fq -- '$StartInfo.Environment["RATELIMITLY_AUTH_KEY"] = $AuthKey' \
  "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner does not construct an explicit child environment"
grep -Fq -- '$StartInfo.Environment.Remove($Name)' \
  "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 child environment retains discovery overrides"
grep -Fq -- '$Client.Start()' "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner does not use direct process creation"
grep -Fq -- 'ReadToEndAsync()' "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner can deadlock on redirected output"
if grep -Fq -- '-FilePath $ExamplePath' \
    "$PRODUCTION_P0_WIN32_NATIVE_RUNNER"; then
  fail "native Win32 P0 runner still launches through Start-Process"
fi
grep -Fq -- 'Start-Sleep -Seconds 11' "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner does not expire stale tracker state"
for variable in \
  RATELIMITLY_TENANT \
  RATELIMITLY_EXAMPLE_SERVER_HOST \
  RATELIMITLY_EXAMPLE_SERVER_PORT; do
  grep -Fq -- "\"$variable\"" "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
    || fail "native Win32 P0 runner does not clear $variable"
done
grep -Fq -- 'inventory response prepared by Win32' \
  "$PRODUCTION_P0_WIN32_NATIVE_RUNNER" \
  || fail "native Win32 P0 runner does not assert protected work"
if grep -Eq -- 'c-2213169720275691601|s-408232124743711' \
    "$PRODUCTION_P0_WIN32_NATIVE_RUNNER"; then
  fail "native Win32 P0 runner hard-codes one production endpoint"
fi
for scenario in guard-pass deny guard-deny; do
  grep -Fq -- "Name = \"$scenario\"" "$WINDOWS_NATIVE_RUNNER" \
    || fail "native Windows behavioral runner omits $scenario"
done
grep -Fq -- 'matches_previous_guard' "$WINDOWS_NATIVE_RUNNER" \
  || fail "native Windows runner does not pair reports with guards"
grep -Fq -- 'CMAKE_C_COMPILER_ID -ne "MSVC"' "$CI_WORKFLOW" \
  || fail "native Windows CI does not verify the Microsoft C compiler"
grep -Fq -- 'cmake -S examples/mongoose' "$CI_WORKFLOW" \
  || fail "native Windows CI does not build a portable framework example"
for windows_example in win32 libuv libevent glib libhv mongoose llhttp; do
  windows_cmake="$ROOT/examples/$windows_example/CMakeLists.txt"
  grep -Fq -- 'add_library(rclient STATIC' "$windows_cmake" \
    || fail "$windows_example CMake does not compile rl-c-client with the target compiler"
  grep -Fq -- 'src/r_client_runtime.c' "$windows_cmake" \
    || fail "$windows_example CMake omits the public runtime implementation"
  grep -Fq -- 'target_link_libraries(rclient PUBLIC OpenSSL::Crypto' "$windows_cmake" \
    || fail "$windows_example does not propagate OpenSSL to the client target"
  if grep -Fq -- 'STATIC IMPORTED' "$windows_cmake"; then
    fail "$windows_example CMake still imports a compiler-specific client archive"
  fi
done
grep -Fq -- '-lssl' "$H2O_MAKEFILE" \
  || fail "H2O Makefile omits libh2o's OpenSSL dependency"
grep -Fq -- '-lm' "$H2O_MAKEFILE" \
  || fail "H2O Makefile omits libh2o's math dependency"
grep -Fq -- 'OpenSSL::SSL' "$H2O_CMAKE" \
  || fail "H2O CMake omits libh2o's OpenSSL dependency"
grep -Eq -- '(^|[[:space:]])m([[:space:]\)]|$)' "$H2O_CMAKE" \
  || fail "H2O CMake omits libh2o's math dependency"
grep -Fq -- 'LWAN_DEP_LIBS' "$LWAN_MAKEFILE" \
  || fail "Lwan Makefile ignores build-specific optional dependencies"
grep -Fq -- 'pkg_check_modules(LWAN_BUILD_CONFIG' "$LWAN_CMAKE" \
  || fail "Lwan CMake ignores build-specific optional dependencies"
grep -Fq -- 'g_io_channel_unref(channel);' "$GLIB_SOURCE" \
  || fail "GLib leaks a channel when source creation fails"
grep -Fq -- 'r_client_format_default_tenant_dns(' "$PERF_SOURCE" \
  || fail "perf client does not use key-derived production DNS"
if grep -Fq -- 'glar.com' "$PERF_SOURCE"; then
  fail "perf client still uses its legacy DNS default"
fi
[[ ! -e "$ROOT/examples/common" ]] \
  || fail "legacy examples/common adapter still exists"
[[ ! -e "$ROOT/tests/test_example_common.c" \
  && ! -e "$ROOT/tests/test_example_common.sh" ]] \
  || fail "legacy common-adapter tests still exist"
if grep -R -E 'common/rl_example|test_example_common' \
  "$ROOT/Makefile" "$ROOT/README.md" "$ROOT/docs" "$ROOT/examples" \
  >/dev/null; then
  fail "build or documentation still references the legacy common adapter"
fi
for migrated in latency_tracker libuv libevent glib libev sd_event kqueue libdispatch win32 libhv epoll liburing io_uring mongoose civetweb libmicrohttpd h2o lwan libreactor facil_io onion kore ulfius llhttp; do
  [[ -d "$ROOT/examples/$migrated" ]] \
    || fail "$migrated does not have its own directory"
done

while IFS='|' read -r name kind marker; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  example_dir="$ROOT/examples/$name"
  source_file="$example_dir/main.c"
  source_files=("$example_dir"/*.c)
  for required in main.c README.md CMakeLists.txt Makefile; do
    [[ -f "$example_dir/$required" ]] \
      || fail "$name is missing $required"
  done
  grep -Fq -- '## Control flow' "$example_dir/README.md" \
    || fail "$name README has no control-flow diagram section"
  grep -Fq -- '```mermaid' "$example_dir/README.md" \
    || fail "$name README has no Mermaid diagram"
  [[ "$(grep -Fc -- '```mermaid' "$example_dir/README.md")" -eq 1 ]] \
    || fail "$name README must contain exactly one Mermaid diagram"
  grep -Eq -- '^flowchart (TD|LR)$' "$example_dir/README.md" \
    || fail "$name README Mermaid diagram has no flowchart direction"
  diagram="$(sed -n '/^```mermaid$/,/^```$/p' "$example_dir/README.md")"
  grep -Eqi -- 'resource|rate limit' <<<"$diagram" \
    || fail "$name README diagram omits resource rate limiting"
  grep -Fqi -- 'latency' <<<"$diagram" \
    || fail "$name README diagram omits latency admission"
  grep -Eqi -- 'report|sample' <<<"$diagram" \
    || fail "$name README diagram omits latency reporting behavior"
  grep -Eq '^## Platform' "$example_dir/README.md" \
    || fail "$name README has no platform-support section"
  grep -Fq -- '## API references' "$example_dir/README.md" \
    || fail "$name README has no official API-reference section"
  grep -Eq -- '\[[^]]+\]\(https?://[^)]+\)' "$example_dir/README.md" \
    || fail "$name README has no online upstream API reference"
  grep -Eqi 'resource|rate limit' "$example_dir/README.md" \
    || fail "$name README does not explain rate limiting"
  grep -Eqi 'latency' "$example_dir/README.md" \
    || fail "$name README does not explain latency tracking"
  grep -Fq -- 'p0.ratelimitly.com' "$example_dir/README.md" \
    || fail "$name README does not explain production DNS discovery"
  grep -Fq -- '`RATELIMITLY_TENANT`' "$example_dir/README.md" \
    || fail "$name README does not explain the tenant DNS override"
  grep -Fq -- "($name/)" "$README" \
    || fail "integration guide does not link the $name folder"
  grep -Fq -- '#include "r_client_runtime.h"' "${source_files[@]}" \
    || fail "$name does not use the public runtime"
  grep -Fq -- '#include "r_client_workflow.h"' "${source_files[@]}" \
    || fail "$name does not use the admission workflow"
  if ! grep -Fq -- 'r_client_admission_report_latency(' "${source_files[@]}" \
    && ! grep -Fq -- 'r_runtime_admission_run_and_report(' "${source_files[@]}"; then
    fail "$name does not report protected-work latency"
  fi
  if grep -En -- '#include[[:space:]]+[<"].*src/' "${source_files[@]}" >/dev/null; then
    fail "$name includes private src header"
  fi
  grep -Fq -- "$marker" "${source_files[@]}" \
    || fail "$name does not use expected $marker API"
  grep -Fq -- ' * Flow' "${source_files[@]}" \
    || fail "$name does not explain its control flow"
  grep -Fq -- ' * Ownership:' "${source_files[@]}" \
    || fail "$name does not explain resource ownership"

  case "$kind" in
    loop|framework)
      symbols=(
        'r_client_admission_start('
        'r_runtime_client_on_readable('
      )
      for symbol in "${symbols[@]}"; do
        grep -Fq -- "$symbol" "$source_file" \
          || fail "$name does not wire $symbol"
      done
      if ! grep -Fq -- 'r_client_admission_deadline_ms(' "$source_file" \
        && ! grep -Fq -- 'r_runtime_admission_delay_ms(' "$source_file"; then
        fail "$name does not wire an admission deadline"
      fi
      if ! grep -Fq -- 'r_client_admission_on_timeout(' "$source_file" \
        && ! grep -Fq -- 'r_runtime_admission_on_timeout(' "$source_file"; then
        fail "$name does not wire admission timeouts"
      fi
      ;;
    parser)
      for symbol in \
        'r_client_admission_start(' \
        'r_runtime_client_on_readable(' \
        'r_runtime_admission_delay_ms(' \
        'r_runtime_admission_on_timeout('; do
        grep -Fq -- "$symbol" "${source_files[@]}" \
          || fail "$name does not wire $symbol"
      done
      ;;
    workflow)
      for symbol in \
        'r_client_admission_start(' \
        'r_client_admission_report_latency(' \
        'r_runtime_client_on_readable(' \
        'r_client_admission_deadline_ms(' \
        'r_client_admission_on_timeout('; do
        grep -Fq -- "$symbol" "$source_file" \
          || fail "$name does not wire $symbol"
      done
      ;;
    *)
      fail "$name has unknown kind: $kind"
      ;;
  esac
done <"$MANIFEST"

# CivetWeb may still enter request handlers until mg_stop() joins its workers.
# The shared bridge must therefore outlive the server worker pool.
civet_source="$ROOT/examples/civetweb/main.c"
server_stop_line="$(grep -nF -- 'mg_stop(server);' "$civet_source" | cut -d: -f1)"
bridge_stop_line="$(grep -nF -- 'bridge_stop(&bridge);' "$civet_source" | tail -1 | cut -d: -f1)"
[[ -n "$server_stop_line" && -n "$bridge_stop_line" \
  && "$server_stop_line" -lt "$bridge_stop_line" ]] \
  || fail "CivetWeb bridge is destroyed before server workers stop"

# Dispatch source cancellation is asynchronous. The descriptors monitored by
# socket sources must remain valid until every cancellation handler has run.
dispatch_source="$ROOT/examples/libdispatch/main.c"
dispatch_readme="$ROOT/examples/libdispatch/README.md"
grep -Fq -- 'dispatch_source_set_cancel_handler_f' "$dispatch_source" \
  || fail "libdispatch sources have no cancellation handlers"
grep -Fq -- 'cancellation handlers have run' "$dispatch_readme" \
  || fail "libdispatch README does not explain safe descriptor teardown"
cancel_wait_line="$(grep -nF -- \
  'dispatch_group_wait(app.source_cancellations' "$dispatch_source" \
  | cut -d: -f1)"
runtime_destroy_line="$(grep -nF -- \
  'dispatch_sync_f(app.queue, &app, destroy_runtime_on_queue);' \
  "$dispatch_source" | cut -d: -f1)"
[[ -n "$cancel_wait_line" && -n "$runtime_destroy_line" \
  && "$cancel_wait_line" -lt "$runtime_destroy_line" ]] \
  || fail "libdispatch runtime is destroyed before source cancellation finishes"

[[ -f "$ROOT/examples/llhttp/llhttp_adapter.h" ]] \
  || fail "llhttp adapter has no host-facing header"
grep -Fq -- '#include "llhttp_adapter.h"' "$ROOT/examples/llhttp/main.c" \
  || fail "llhttp source does not use its host-facing header"

# Keep the adoption guide aligned with the supported inventory and present the
# examples in the same progression: loops, frameworks, then parser-only code.
expected_headings=$'libuv\nlibevent\nGLib/GIO\nlibev\nsd-event (Linux)\nkqueue (macOS/BSD)\nlibdispatch\nWin32\nlibhv\nliburing (Linux)\nepoll (Linux)\nio_uring without liburing (Linux)\nMongoose\nCivetWeb\nGNU libmicrohttpd\nH2O\nLwan\nlibreactor\nfacil.io\nOnion\nKore\nUlfius'
actual_headings="$(sed -n 's/^### //p' "$README")"
[[ "$actual_headings" == "$expected_headings" ]] \
  || fail "README example headings are missing or out of order"
grep -Fq -- 'If it returns `HPE_PAUSED`' "$README" \
  || fail "README does not explain resumable llhttp backpressure"
grep -Fq -- '## Latency tracking workflow' "$README" \
  || fail "README does not document latency tracking"
grep -Fq -- '## Platform matrix' "$README" \
  || fail "README does not provide a platform matrix"
grep -Fq -- '```mermaid' "$README" \
  || fail "README does not provide a Mermaid integration overview"
grep -Fq -- 'Never report latency for work rejected by the guard.' "$README" \
  || fail "README does not explain denied latency-guard behavior"

grep -Fq -- '[examples/README.md](examples/README.md)' "$ROOT_README" \
  || fail "root README does not link the integration guide"
grep -Fq -- '`result->success` combines resource and latency-guard decisions' \
  "$API_GUIDE" \
  || fail "API guide does not explain combined admission results"
grep -Fq -- '`r_client_runtime.h`' "$API_GUIDE" \
  || fail "API guide does not document the public runtime header"
grep -Fq -- '`r_client_workflow.h`' "$API_GUIDE" \
  || fail "API guide does not document the public workflow header"
if grep -Fq -- '../examples/latency_tracker.c' "$API_GUIDE"; then
  fail "API guide links the removed flat latency example"
fi
grep -Fq -- 'Never report latency for work rejected by its guard.' "$API_GUIDE" \
  || fail "API guide does not explain denied latency-guard behavior"
grep -Fq -- 'The report must repeat the guard' "$API_GUIDE" \
  || fail "API guide does not explain tracker identity"
grep -Fq -- '## Clock domains' "$IO_GUIDE" \
  || fail "I/O guide does not distinguish client and measurement clocks"
grep -Fq -- '`CLOCK_MONOTONIC`' "$IO_GUIDE" \
  || fail "I/O guide does not specify monotonic latency measurement"

grep -Fq -- 'clock_gettime(CLOCK_MONOTONIC' \
  "$ROOT/examples/latency_tracker/main.c" \
  || grep -Fq -- 'r_runtime_monotonic_time_ms(' \
    "$ROOT/examples/latency_tracker/main.c" \
  || fail "latency workflow does not use monotonic duration time"
