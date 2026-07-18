#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX="$ROOT/tests/macos-local-examples.txt"
RUNNER="$ROOT/tests/run_one_shot_example.sh"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "test_macos_examples: SKIP (run locally on macOS)"
  exit 0
fi

# This suite deliberately stays out of CI. It exercises APIs supplied by the
# local macOS kernel and libdispatch installation against the same authenticated
# responder contract as the portable Linux examples.
make -C "$ROOT" clean
make -C "$ROOT" \
  CFLAGS='-O2 -Wall -Wextra -Werror -std=c11' \
  librclient.a test-responder

base_port=${R_MACOS_EXAMPLE_PORT_BASE:-39800}
while IFS='|' read -r name binary profile metrics_label; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  example_dir="$ROOT/$(dirname "$binary")"
  make -C "$example_dir" clean all
  "$RUNNER" "$name" "$binary" "$profile" "$metrics_label" "$base_port"
  base_port=$((base_port + 10))
done <"$MATRIX"

echo "test_macos_examples: PASS"
