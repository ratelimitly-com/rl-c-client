#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
AUDIT_CLIENT="$ROOT/bin/audit_client"
TENANT_SECRET='rl-cookie1credential-shaped-material-that-must-not-appear'
MANAGEMENT_SECRET='rl-secret1management-material-that-must-not-appear'
R_TEST_RESPONDER_AES_KEY='rl-aes1qvqqqqqqqqqqqqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqvpsxqcrqqqqzqqqqsqqqqqsqqqyqqqqqqkqzqqqhmzd8l'

fail() {
  echo "test_audit_client_cli: $*" >&2
  exit 1
}

help=$($AUDIT_CLIENT --help)
[[ $help == *'Usage: audit_client --auth=<bech32> [OPTIONS]'* ]] \
  || fail "help does not describe the mandatory tenant credential"
[[ $help == *'--management-key=<bech32>'* ]] \
  || fail "help does not describe optional metrics authentication"
[[ $help == *'--replay-schedule=<kind>'* ]] \
  || fail "help does not expose the replay schedule"
[[ $help == *'--final-receive-units=<n>'* ]] \
  || fail "help does not expose the final receive interval"
[[ $help == *'--completion-delivery=<bool>'* ]] \
  || fail "help does not expose completion delivery"
[[ $help != *'--srv'* && $help != *'--server-host'* && $help != *'--server-port'* ]] \
  || fail "help exposes a DNS or fixed-endpoint override"

set +e
output=$($AUDIT_CLIENT 2>&1)
status=$?
set -e
[[ $status -eq 2 ]] || fail "missing --auth did not exit 2"
[[ $output == *'Missing required --auth'* ]] \
  || fail "missing-auth diagnostic is absent"

set +e
output=$($AUDIT_CLIENT --auth="$TENANT_SECRET" 2>&1)
status=$?
set -e
[[ $status -eq 2 ]] || fail "invalid --auth did not exit 2"
[[ $output == *'Invalid --auth'* ]] \
  || fail "invalid-auth diagnostic is absent"
[[ $output != *"$TENANT_SECRET"* ]] \
  || fail "invalid-auth diagnostic exposed the tenant credential"

set +e
output=$($AUDIT_CLIENT \
  --auth="$R_TEST_RESPONDER_AES_KEY" \
  --management-key="$MANAGEMENT_SECRET" 2>&1)
status=$?
set -e
[[ $status -eq 2 ]] || fail "invalid management key did not exit 2"
[[ $output == *'Invalid --management-key'* ]] \
  || fail "invalid-management-key diagnostic is absent"
[[ $output != *"$MANAGEMENT_SECRET"* ]] \
  || fail "invalid-management-key diagnostic exposed the credential"

set +e
output=$($AUDIT_CLIENT \
  --auth="$R_TEST_RESPONDER_AES_KEY" \
  --replay-schedule=linear \
  --replay-growth=0 2>&1)
status=$?
set -e
[[ $status -eq 2 ]] || fail "invalid linear schedule did not exit 2"
[[ $output == *'Invalid HA policy'* ]] \
  || fail "invalid-policy diagnostic is absent"

set +e
output=$($AUDIT_CLIENT --auth="$R_TEST_RESPONDER_AES_KEY" --unknown 2>&1)
status=$?
set -e
[[ $status -eq 2 ]] || fail "unknown option did not exit 2"
[[ $output == *'Unknown option'* ]] \
  || fail "unknown-option diagnostic is absent"

for removed_option in --srv=example.invalid --server-host=127.0.0.1 --server-port=8080; do
  set +e
  output=$($AUDIT_CLIENT --auth="$R_TEST_RESPONDER_AES_KEY" "$removed_option" 2>&1)
  status=$?
  set -e
  [[ $status -eq 2 ]] || fail "$removed_option did not exit 2"
  [[ $output == *'Unknown option'* ]] \
    || fail "$removed_option is still accepted"
done
