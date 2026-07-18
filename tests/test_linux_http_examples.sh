#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX="$ROOT/tests/linux-http-examples.txt"
RUNNER="$ROOT/tests/run_http_example.sh"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "test_linux_http_examples: SKIP (Linux-only matrix)"
  exit 0
fi
if [[ "$#" -ne 1 || ! "$1" =~ ^(a|b|c|all)$ ]]; then
  echo "usage: $0 <a|b|c|all>" >&2
  exit 2
fi

shard=$1
base_port=${R_LINUX_HTTP_UDP_PORT_BASE:-41400}
while IFS='|' read -r name row_shard binary http_port metrics_label deny_status launcher; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  if [[ "$shard" != "all" && "$row_shard" != "$shard" ]]; then
    continue
  fi
  "$RUNNER" \
    "$name" "$binary" "$http_port" "$metrics_label" \
    "$deny_status" "$launcher" "$base_port"
  base_port=$((base_port + 10))
done <"$MATRIX"

echo "test_linux_http_examples: PASS ($shard)"
