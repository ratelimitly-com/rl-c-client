#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PERF_CLIENT="$ROOT/bin/perf_client"
SECRET='rl-cookie1credential-shaped-material-that-must-not-appear'

fail() {
  echo "test_perf_client_cli: $*" >&2
  exit 1
}

set +e
output=$($PERF_CLIENT --auth="$SECRET" 2>&1)
status=$?
set -e

[[ $status -eq 2 ]] || fail "invalid authentication value did not exit 2"
[[ $output == *'Invalid --auth value'* ]] \
  || fail "invalid authentication diagnostic is missing"
[[ $output != *"$SECRET"* ]] \
  || fail "invalid authentication diagnostic exposed the credential"
