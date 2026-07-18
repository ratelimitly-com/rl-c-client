#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MATRIX="$ROOT/tests/linux-one-shot-examples.txt"
TEST_NAME=test_production_p0_one_shot_examples

fail() {
  echo "$TEST_NAME: $*" >&2
  exit 1
}

if [[ $(uname -s) != Linux ]]; then
  echo "$TEST_NAME: SKIP (Linux-only production matrix)"
  exit 0
fi

# The production credential must arrive through the runner environment. Never
# place it in an argument, command trace, temporary file, or diagnostic output.
[[ -n ${RATELIMITLY_AUTH_KEY:-} ]] \
  || fail "RATELIMITLY_AUTH_KEY is required"
export RATELIMITLY_AUTH_KEY

# Production P0 discovery is derived from the authentication key. Reject a
# non-empty override, then remove even an empty inherited value so every child
# exercises SRV discovery under c-<key-id>.p0.ratelimitly.com.
for variable in \
  RATELIMITLY_TENANT \
  RATELIMITLY_EXAMPLE_SERVER_HOST \
  RATELIMITLY_EXAMPLE_SERVER_PORT; do
  [[ -z ${!variable:-} ]] \
    || fail "$variable must not override key-derived production discovery"
  unset "$variable"
done

command -v timeout >/dev/null 2>&1 \
  || fail "GNU timeout is required"
[[ -r $MATRIX ]] || fail "missing matrix: $MATRIX"

declare -a example_names=()
declare -a example_binaries=()
declare -a example_profiles=()

# Validate the whole matrix before waiting for stale server state to expire.
# This keeps missing dependencies and malformed entries fast and actionable.
while IFS='|' read -r name binary profile metrics_label extra; do
  [[ -z $name || $name == \#* ]] && continue
  [[ -z ${extra:-} && -n $binary && -n $profile && -n $metrics_label ]] \
    || fail "malformed matrix entry for $name"
  [[ $name =~ ^[a-z0-9_]+$ ]] \
    || fail "unsafe example name in matrix"
  case $profile in
    latency|loop|parser) ;;
    *) fail "$name has unknown output profile: $profile" ;;
  esac

  if [[ $binary != /* ]]; then
    binary="$ROOT/$binary"
  fi
  [[ -x $binary ]] || fail "$name executable is missing: $binary"

  example_names+=("$name")
  example_binaries+=("$binary")
  example_profiles+=("$profile")
done <"$MATRIX"

[[ ${#example_names[@]} -gt 0 ]] || fail "matrix contains no examples"

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/r-p0-one-shot.XXXXXX")
chmod 700 "$TMP_DIR"
cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Trackers retain samples for 10 seconds. CI serializes users of this shared
# production credential; wait once, before the matrix, so a cancelled or failed
# predecessor cannot make the first guard observe stale latency. Do not repeat
# this delay per example: every example has its own bucket and service identity.
echo "$TEST_NAME: draining stale production state (11 seconds)"
sleep 11

sanitize_file() {
  local file=$1
  local line redacted token
  local count=0
  while IFS= read -r line || [[ -n $line ]]; do
    redacted=${line//"$RATELIMITLY_AUTH_KEY"/[REDACTED]}
    # Also redact credential-shaped text in case a dependency prints a parsed
    # or reconstructed key rather than the exact inherited byte string.
    while [[ $redacted =~ rl-(aes|hmac)[[:alnum:]]{8,} ]]; do
      token=${BASH_REMATCH[0]}
      redacted=${redacted//"$token"/[REDACTED]}
    done
    printf '%s\n' "$redacted" >&2
    count=$((count + 1))
    if ((count == 100)); then
      echo "[diagnostic output truncated after 100 lines]" >&2
      break
    fi
  done <"$file"
}

fail_example() {
  local name=$1
  local message=$2
  local stdout_file=$3
  local stderr_file=$4

  echo "$TEST_NAME/$name: $message" >&2
  if [[ -s $stdout_file ]]; then
    echo "--- sanitized stdout" >&2
    sanitize_file "$stdout_file"
  fi
  if [[ -s $stderr_file ]]; then
    echo "--- sanitized stderr" >&2
    sanitize_file "$stderr_file"
  fi
  exit 1
}

assert_example_output() {
  local name=$1
  local profile=$2
  local stdout_file=$3
  local stderr_file=$4
  local -a lines=()
  local number_pattern='^[0-9]+$'
  local positive_number_pattern='^[1-9][0-9]*$'
  local value

  [[ ! -s $stderr_file ]] \
    || fail_example "$name" "unexpected stderr" "$stdout_file" "$stderr_file"
  mapfile -t lines <"$stdout_file"

  case $profile in
    loop)
      [[ ${#lines[@]} -eq 1 \
          && ${lines[0]} =~ ^allowed:\ .+\;\ latency=([0-9]+)\ ms$ \
          && ${BASH_REMATCH[1]} =~ $number_pattern ]] \
        || fail_example "$name" \
          "expected exactly one documented allowed/latency line" \
          "$stdout_file" "$stderr_file"
      ;;
    latency)
      if [[ ${#lines[@]} -eq 2 \
            && ${lines[0]} == \
              'guard passed: resource and latency checks admitted the work' \
            && ${lines[1]} =~ \
              ^latency\ reported:\ service=example-inventory-backend\ observed=([0-9]+)\ ms$ ]]; then
        value=${BASH_REMATCH[1]}
      else
        fail_example "$name" \
          "expected exactly the documented admission and latency lines" \
          "$stdout_file" "$stderr_file"
      fi
      [[ $value =~ $positive_number_pattern ]] \
        || fail_example "$name" \
          "expected the documented admission and positive latency report" \
          "$stdout_file" "$stderr_file"
      ;;
    parser)
      [[ ${#lines[@]} -eq 3 \
          && ${lines[0]} == 'protected work: GET /limited' \
          && ${lines[1]} == 'decision: allowed' \
          && ${lines[2]} =~ ^reported\ latency:\ ([0-9]+)\ ms$ \
          && ${BASH_REMATCH[1]} =~ $number_pattern ]] \
        || fail_example "$name" \
          "expected exactly the documented parser, allow, and latency lines" \
          "$stdout_file" "$stderr_file"
      ;;
  esac
}

run_example() {
  local name=$1
  local binary=$2
  local profile=$3
  local index=$4
  local binary_dir binary_name stdout_file stderr_file status

  binary_dir=$(dirname "$binary")
  binary_name=$(basename "$binary")
  stdout_file="$TMP_DIR/$index-$name.stdout"
  stderr_file="$TMP_DIR/$index-$name.stderr"

  status=0
  (
    cd "$binary_dir"
    unset RATELIMITLY_TENANT
    unset RATELIMITLY_EXAMPLE_SERVER_HOST
    unset RATELIMITLY_EXAMPLE_SERVER_PORT
    # A positive duration makes the standalone latency-track example prove it
    # reports measured work, while the other examples retain their own work.
    export RATELIMITLY_EXAMPLE_WORK_MS=25
    # Start with the normal `timeout 29s` TERM, then force cleanup one second
    # later if an example ignores it. The total process deadline remains 30s.
    timeout --signal=TERM --kill-after=1s 29s "./$binary_name"
  ) >"$stdout_file" 2>"$stderr_file" || status=$?

  [[ $status -eq 0 ]] \
    || fail_example "$name" \
      "example exited $status; expected 0 (deadline is 30 seconds)" \
      "$stdout_file" "$stderr_file"
  assert_example_output "$name" "$profile" "$stdout_file" "$stderr_file"
  echo "$TEST_NAME/$name: PASS"
}

for index in "${!example_names[@]}"; do
  run_example \
    "${example_names[index]}" \
    "${example_binaries[index]}" \
    "${example_profiles[index]}" \
    "$index"
done

echo "$TEST_NAME: PASS (${#example_names[@]} production P0 examples)"
