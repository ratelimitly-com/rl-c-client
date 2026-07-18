#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PROBE="$ROOT/bin/production_p0_probe"

fail() {
  echo "test_production_p0: $*" >&2
  exit 1
}

[[ -n "${RATELIMITLY_AUTH_KEY:-}" ]] \
  || fail "RATELIMITLY_AUTH_KEY is required"

namespace=${RATELIMITLY_P0_TEST_NAMESPACE:-}
[[ -n "$namespace" ]] \
  || fail "RATELIMITLY_P0_TEST_NAMESPACE is required"
[[ ${#namespace} -le 48 && "$namespace" =~ ^[A-Za-z0-9_-]+$ ]] \
  || fail "RATELIMITLY_P0_TEST_NAMESPACE must be 1..48 safe ASCII characters"

# Production discovery must be derived exclusively from the credential. Refuse
# accidental overrides, then remove even empty values from the child process.
for variable in \
  RATELIMITLY_TENANT \
  RATELIMITLY_EXAMPLE_SERVER_HOST \
  RATELIMITLY_EXAMPLE_SERVER_PORT; do
  [[ -z "${!variable:-}" ]] \
    || fail "$variable must not override key-derived production discovery"
done
unset RATELIMITLY_TENANT
unset RATELIMITLY_EXAMPLE_SERVER_HOST
unset RATELIMITLY_EXAMPLE_SERVER_PORT

command -v timeout >/dev/null 2>&1 \
  || fail "the POSIX probe requires the timeout command"

make -C "$ROOT" production-p0-probe
timeout 60s "$PROBE"
