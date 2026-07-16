#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/examples/manifest.txt"
README="${R_EXAMPLE_README_PATH:-$ROOT/examples/README.md}"

fail() {
  echo "test_examples: $*" >&2
  exit 1
}

[[ -f "$MANIFEST" ]] || fail "missing examples/manifest.txt"

while IFS='|' read -r name kind marker; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  source_file="$ROOT/examples/$name.c"
  [[ -f "$source_file" ]] || fail "missing examples/$name.c"

  grep -Fq -- '#include "common/rl_example.h"' "$source_file" \
    || fail "$name does not use shared public adapter"
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
      grep -Fq -- 'rl_example_check(' "$source_file" \
        || fail "$name does not submit rate-limit checks"
      for symbol in \
        'rl_example_client_on_readable(' \
        'rl_example_request_delay_ms(' \
        'rl_example_request_on_timeout('; do
        grep -Fq -- "$symbol" "$source_file" \
          || fail "$name does not wire $symbol"
      done
      ;;
    parser)
      grep -Fq -- 'rl_example_check(' "$source_file" \
        || fail "$name does not submit rate-limit checks"
      ;;
    workflow)
      for symbol in \
        'r_client_check_rate_limit_async_borrowed(' \
        'r_client_report_latency(' \
        'rl_example_client_on_readable(' \
        'rl_example_request_delay_ms(' \
        'rl_example_request_on_timeout('; do
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
