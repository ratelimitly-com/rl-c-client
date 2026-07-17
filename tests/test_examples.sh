#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/examples/manifest.txt"
README="${R_EXAMPLE_README_PATH:-$ROOT/examples/README.md}"
ROOT_README="$ROOT/README.md"
API_GUIDE="$ROOT/docs/api.md"
IO_GUIDE="$ROOT/IO_ABSTRACTION.md"
COMMON_ADAPTER="$ROOT/examples/common/rl_example.c"

fail() {
  echo "test_examples: $*" >&2
  exit 1
}

[[ -f "$MANIFEST" ]] || fail "missing examples/manifest.txt"
for migrated in latency_tracker libuv libevent epoll; do
  [[ -d "$ROOT/examples/$migrated" ]] \
    || fail "$migrated does not have its own directory"
done

while IFS='|' read -r name kind marker; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  example_dir="$ROOT/examples/$name"
  if [[ -d "$example_dir" ]]; then
    source_file="$example_dir/main.c"
    for required in main.c README.md CMakeLists.txt Makefile; do
      [[ -f "$example_dir/$required" ]] \
        || fail "$name is missing $required"
    done
    if grep -Fq -- 'common/rl_example' "$source_file"; then
      fail "$name still depends on examples/common"
    fi
    grep -Fq -- '#include "r_client_runtime.h"' "$source_file" \
      || fail "$name does not use the public runtime"
    grep -Fq -- '#include "r_client_workflow.h"' "$source_file" \
      || fail "$name does not use the admission workflow"
    if ! grep -Fq -- 'r_client_admission_report_latency(' "$source_file" \
      && ! grep -Fq -- 'r_runtime_admission_run_and_report(' "$source_file"; then
      fail "$name does not report protected-work latency"
    fi
  else
    source_file="$ROOT/examples/$name.c"
    [[ -f "$source_file" ]] || fail "missing examples/$name.c"
    grep -Fq -- '#include "common/rl_example.h"' "$source_file" \
      || fail "$name does not use shared public adapter"
  fi
  if grep -En -- '#include[[:space:]]+[<"].*src/' "$source_file" >/dev/null; then
    fail "$name includes private src header"
  fi
  grep -Fq -- "$marker" "$source_file" \
    || fail "$name does not use expected $marker API"
  grep -Fq -- ' * Flow' "$source_file" \
    || fail "$name does not explain its control flow"
  grep -Fq -- ' * Ownership:' "$source_file" \
    || fail "$name does not explain resource ownership"

  case "$kind" in
    loop|framework)
      if [[ -d "$example_dir" ]]; then
        symbols=(
          'r_client_admission_start('
          'r_runtime_client_on_readable('
        )
      else
        symbols=(
          'rl_example_check('
          'rl_example_client_on_readable('
          'rl_example_request_delay_ms('
          'rl_example_request_on_timeout('
        )
      fi
      for symbol in "${symbols[@]}"; do
        grep -Fq -- "$symbol" "$source_file" \
          || fail "$name does not wire $symbol"
      done
      if [[ -d "$example_dir" ]]; then
        if ! grep -Fq -- 'r_client_admission_deadline_ms(' "$source_file" \
          && ! grep -Fq -- 'r_runtime_admission_delay_ms(' "$source_file"; then
          fail "$name does not wire an admission deadline"
        fi
        if ! grep -Fq -- 'r_client_admission_on_timeout(' "$source_file" \
          && ! grep -Fq -- 'r_runtime_admission_on_timeout(' "$source_file"; then
          fail "$name does not wire admission timeouts"
        fi
      fi
      ;;
    parser)
      grep -Fq -- 'rl_example_check(' "$source_file" \
        || fail "$name does not submit rate-limit checks"
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
civet_source="$ROOT/examples/civetweb.c"
server_stop_line="$(grep -nF -- 'mg_stop(server);' "$civet_source" | cut -d: -f1)"
bridge_stop_line="$(grep -nF -- 'bridge_stop(&bridge);' "$civet_source" | tail -1 | cut -d: -f1)"
[[ -n "$server_stop_line" && -n "$bridge_stop_line" \
  && "$server_stop_line" -lt "$bridge_stop_line" ]] \
  || fail "CivetWeb bridge is destroyed before server workers stop"

[[ -f "$ROOT/examples/llhttp_adapter.h" ]] \
  || fail "llhttp adapter has no host-facing header"
grep -Fq -- '#include "llhttp_adapter.h"' "$ROOT/examples/llhttp.c" \
  || fail "llhttp source does not use its host-facing header"

# Keep the adoption guide aligned with the supported inventory and present the
# examples in the same progression: loops, frameworks, then parser-only code.
expected_headings=$'libuv\nlibevent\nlibhv\nliburing (Linux)\nepoll (Linux)\nio_uring without liburing (Linux)\nMongoose\nCivetWeb\nGNU libmicrohttpd\nH2O\nLwan\nlibreactor\nfacil.io\nOnion\nKore\nUlfius'
actual_headings="$(sed -n 's/^### //p' "$README")"
[[ "$actual_headings" == "$expected_headings" ]] \
  || fail "README example headings are missing or out of order"
grep -Fq -- 'If it returns `HPE_PAUSED`' "$README" \
  || fail "README does not explain resumable llhttp backpressure"
grep -Fq -- '## Latency tracking workflow' "$README" \
  || fail "README does not document latency tracking"
grep -Fq -- 'Never report latency for work rejected by the guard.' "$README" \
  || fail "README does not explain denied latency-guard behavior"

grep -Fq -- '[examples/README.md](examples/README.md)' "$ROOT_README" \
  || fail "root README does not link the integration guide"
grep -Fq -- '`result->success` combines resource and latency-guard decisions' \
  "$API_GUIDE" \
  || fail "API guide does not explain combined admission results"
grep -Fq -- 'Never report latency for work rejected by its guard.' "$API_GUIDE" \
  || fail "API guide does not explain denied latency-guard behavior"
grep -Fq -- 'The report must repeat the guard' "$API_GUIDE" \
  || fail "API guide does not explain tracker identity"
grep -Fq -- '## Clock domains' "$IO_GUIDE" \
  || fail "I/O guide does not distinguish client and measurement clocks"
grep -Fq -- '`CLOCK_MONOTONIC`' "$IO_GUIDE" \
  || fail "I/O guide does not specify monotonic latency measurement"

grep -Fq -- 'clock_gettime(CLOCK_REALTIME' "$COMMON_ADAPTER" \
  || fail "shared adapter does not provide Unix-epoch client time"
grep -Fq -- 'clock_gettime(CLOCK_MONOTONIC' \
  "$ROOT/examples/latency_tracker/main.c" \
  || grep -Fq -- 'r_runtime_monotonic_time_ms(' \
    "$ROOT/examples/latency_tracker/main.c" \
  || fail "latency workflow does not use monotonic duration time"
