#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX="$ROOT/tests/linux-one-shot-examples.txt"
RUNNER="$ROOT/tests/run_one_shot_example.sh"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "test_linux_one_shot_examples: SKIP (Linux-only matrix)"
  exit 0
fi

base_port=${R_LINUX_ONE_SHOT_PORT_BASE:-39200}
while IFS='|' read -r name binary profile metrics_label; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  "$RUNNER" "$name" "$binary" "$profile" "$metrics_label" "$base_port"
  base_port=$((base_port + 10))
done <"$MATRIX"

echo "test_linux_one_shot_examples: PASS"
