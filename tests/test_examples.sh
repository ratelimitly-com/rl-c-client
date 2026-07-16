#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/examples/manifest.txt"

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
  grep -Fq -- 'rl_example_check(' "$source_file" \
    || fail "$name does not submit rate-limit checks"
  grep -Fq -- "$marker" "$source_file" \
    || fail "$name does not use expected $marker API"

  case "$kind" in
    loop|framework)
      for symbol in \
        'rl_example_client_on_readable(' \
        'rl_example_request_delay_ms(' \
        'rl_example_request_on_timeout('; do
        grep -Fq -- "$symbol" "$source_file" \
          || fail "$name does not wire $symbol"
      done
      ;;
    parser)
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
