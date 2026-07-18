#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/examples/manifest.txt"
README="${R_EXAMPLE_README_PATH:-$ROOT/examples/README.md}"
ROOT_README="$ROOT/README.md"
API_GUIDE="$ROOT/docs/api.md"
IO_GUIDE="$ROOT/IO_ABSTRACTION.md"
CI_WORKFLOW="$ROOT/.github/workflows/ci.yml"
H2O_MAKEFILE="$ROOT/examples/h2o/Makefile"
H2O_CMAKE="$ROOT/examples/h2o/CMakeLists.txt"
LWAN_MAKEFILE="$ROOT/examples/lwan/Makefile"
LWAN_CMAKE="$ROOT/examples/lwan/CMakeLists.txt"
GLIB_SOURCE="$ROOT/examples/glib/main.c"
PERF_SOURCE="$ROOT/bin/perf_client.c"
LINUX_ONE_SHOT_MATRIX="$ROOT/tests/linux-one-shot-examples.txt"
LINUX_HTTP_MATRIX="$ROOT/tests/linux-http-examples.txt"
MACOS_LOCAL_MATRIX="$ROOT/tests/macos-local-examples.txt"

fail() {
  echo "test_examples: $*" >&2
  exit 1
}

[[ -f "$MANIFEST" ]] || fail "missing examples/manifest.txt"
[[ -f "$LINUX_ONE_SHOT_MATRIX" ]] \
  || fail "missing executable Linux one-shot example matrix"
[[ -f "$LINUX_HTTP_MATRIX" ]] \
  || fail "missing executable Linux HTTP example matrix"
[[ -f "$MACOS_LOCAL_MATRIX" ]] \
  || fail "missing local-only macOS example matrix"
grep -Fq -- 'bash tests/test_linux_one_shot_examples.sh' "$CI_WORKFLOW" \
  || fail "CI does not execute Linux one-shot examples"
grep -Fq -- 'bash tests/test_linux_http_examples.sh' "$CI_WORKFLOW" \
  || fail "CI does not execute Linux HTTP examples"
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
grep -Fq -- 'runs-on: windows-latest' "$CI_WORKFLOW" \
  || fail "CI does not validate the Win32 example on native Windows"
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
