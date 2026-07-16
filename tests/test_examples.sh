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

  rg -Fq '#include "common/rl_example.h"' "$source_file" \
    || fail "$name does not use shared public adapter"
  if rg -n '#include[[:space:]]+[<"].*src/' "$source_file" >/dev/null; then
    fail "$name includes private src header"
  fi
  rg -Fq 'rl_example_check(' "$source_file" \
    || fail "$name does not submit rate-limit checks"
  rg -Fq "$marker" "$source_file" \
    || fail "$name does not use expected $marker API"

  case "$kind" in
    loop|framework)
      for symbol in \
        'rl_example_client_on_readable(' \
        'rl_example_request_delay_ms(' \
        'rl_example_request_on_timeout('; do
        rg -Fq "$symbol" "$source_file" \
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
