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

help=$($PERF_CLIENT --help)
[[ $help == *'--target-rps=<n>'* ]] || fail "paced-mode help is missing"
[[ $help == *'--rate-limit=<n>'* ]] || fail "workload rate-limit help is missing"
[[ $help == *'--shared-bucket'* ]] || fail "shared-bucket help is missing"

for invalid_rate in 0 4294967296 invalid; do
  set +e
  output=$($PERF_CLIENT --rate-limit="$invalid_rate" --auth="$SECRET" 2>&1)
  status=$?
  set -e
  [[ $status -eq 2 ]] || fail "invalid rate limit $invalid_rate did not exit 2"
  [[ $output == *'Invalid --rate-limit'* ]] || fail "rate-limit diagnostic is missing"
  [[ $output != *"$SECRET"* ]] || fail "rate-limit diagnostic exposed the credential"
done

set +e
output=$($PERF_CLIENT --target-rps=10000 --duration=0 --auth="$SECRET" 2>&1)
status=$?
set -e
[[ $status -eq 2 ]] || fail "paced mode without a valid duration did not exit 2"
[[ $output == *'Invalid --target-rps'* ]] || fail "paced-mode diagnostic is missing"
[[ $output != *"$SECRET"* ]] || fail "paced-mode diagnostic exposed the credential"

set +e
output=$($PERF_CLIENT --target-rps=10000 --duration=1 --clients=1025 --auth="$SECRET" 2>&1)
status=$?
set -e
[[ $status -eq 2 ]] || fail "unbounded paced-mode client count did not exit 2"
[[ $output == *'--clients=1..1024'* ]] || fail "paced-mode client bound is missing"
