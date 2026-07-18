#!/usr/bin/env bash
set -euo pipefail

MATRIX_AUTH_KEY=${RATELIMITLY_AUTH_KEY:-}
unset RATELIMITLY_AUTH_KEY
export -n MATRIX_AUTH_KEY

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MATRIX="$ROOT/tests/linux-http-examples.txt"
RUNNER="$ROOT/tests/run_production_p0_http_example.sh"

fail() {
  echo "test_production_p0_http_examples: $*" >&2
  exit 1
}

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "test_production_p0_http_examples: SKIP (Linux-only matrix)"
  exit 0
fi
if [[ "$#" -ne 1 || ! "$1" =~ ^(a|b|c|all)$ ]]; then
  echo "usage: $0 <a|b|c|all>" >&2
  exit 2
fi

shard=$1
[[ -n "$MATRIX_AUTH_KEY" ]] \
  || fail "RATELIMITLY_AUTH_KEY is required"
ulimit -c 0 || fail "could not disable core dumps"

# Production discovery is intentionally key-only. Reject an override even when
# it is empty: that catches incorrectly configured CI before the examples run.
for variable in \
  RATELIMITLY_TENANT \
  RATELIMITLY_EXAMPLE_SERVER_HOST \
  RATELIMITLY_EXAMPLE_SERVER_PORT; do
  [[ -z "${!variable+x}" ]] \
    || fail "$variable must be unset for key-derived production discovery"
done
unset RATELIMITLY_TENANT
unset RATELIMITLY_EXAMPLE_SERVER_HOST
unset RATELIMITLY_EXAMPLE_SERVER_PORT

[[ -r "$MATRIX" ]] || fail "missing matrix: $MATRIX"
[[ -x "$RUNNER" ]] || fail "missing executable runner: $RUNNER"
command -v timeout >/dev/null 2>&1 \
  || fail "GNU timeout is required"

selected_rows=()
while IFS='|' read -r \
    name row_shard artifact http_port metrics_label deny_status launcher extra; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  [[ -z "$extra" ]] || fail "malformed matrix row for $name"
  [[ "$row_shard" =~ ^[abc]$ ]] || fail "invalid shard for $name"
  [[ -n "$artifact" && -n "$metrics_label" ]] \
    || fail "incomplete matrix row for $name"
  [[ "$deny_status" =~ ^[1-5][0-9][0-9]$ ]] \
    || fail "invalid denial status for $name"
  [[ "$http_port" =~ ^[0-9]+$ ]] \
    || fail "invalid HTTP port for $name"
  ((http_port >= 1024 && http_port <= 65535)) \
    || fail "HTTP port out of range for $name"
  [[ "$launcher" == "direct" || "$launcher" == "kore" ]] \
    || fail "invalid launcher for $name"

  if [[ "$shard" == "all" || "$row_shard" == "$shard" ]]; then
    selected_rows+=("$name|$artifact|$http_port|$launcher")
  fi
done <"$MATRIX"

((${#selected_rows[@]} > 0)) || fail "shard $shard selected no examples"

# Every example uses a tracker with a 10-second TTL. Wait once before the
# matrix, not between examples: tracker names are framework-specific, and the
# matrix runs one server at a time on its declared local port.
echo "production P0 HTTP: draining stale latency samples for 11 seconds"
sleep 11

for row in "${selected_rows[@]}"; do
  IFS='|' read -r name artifact http_port launcher <<<"$row"
  echo "$name: checking production P0 on local port $http_port"
  runner_status=0
  (
    # Keep the credential out of timeout's environment and argv. The helper
    # consumes descriptor 3 immediately, closes it, then gives only the
    # framework process an authentication environment variable.
    export RATELIMITLY_AUTH_KEY_FD=3
    exec timeout --signal=TERM --kill-after=5s 55s \
      "$RUNNER" "$name" "$artifact" "$http_port" "$launcher" \
      3<<<"$MATRIX_AUTH_KEY"
  ) || runner_status=$?
  ((runner_status == 0)) \
    || fail "$name runner exited with status $runner_status"
done

echo "test_production_p0_http_examples: PASS ($shard)"
