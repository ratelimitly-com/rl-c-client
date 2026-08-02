#!/usr/bin/env bash

# Shared GitHub Actions request profile. Both workflows set these values for
# every test process; production runners also apply them explicitly so direct
# local invocations reproduce Actions and record scheduler-window consumption.
production_p0_apply_request_profile() {
  export RATELIMITLY_REQUEST_UNIT_MS=25
  export RATELIMITLY_REQUEST_REPLAY_COUNT=3
  export RATELIMITLY_REQUEST_PROFILE=1
}

production_p0_report_profiles() {
  local file=$1
  local label=$2
  local expected_count=$3
  local only_profiles=${4:-false}
  local line normalized
  local -a profiles=()
  local all_line_count=0

  [[ -r $file ]] || return 1
  while IFS= read -r line || [[ -n $line ]]; do
    ((all_line_count += 1))
    normalized=${line%$'\r'}
    if [[ $normalized == 'rl-c-client[profile]:'* ]]; then
      profiles+=("$normalized")
    fi
  done <"$file"

  [[ ${#profiles[@]} -eq $expected_count ]] || return 1
  if [[ $only_profiles == true && $all_line_count -ne $expected_count ]]; then
    return 1
  fi
  for line in "${profiles[@]}"; do
    [[ $line =~ ^rl-c-client\[profile\]:\ wait_ms=[0-9]+\ unit_ms=25\ replay_count=3\ round=[0-3]\ phase=(round|final)\ status=0\ response=selected$ ]] \
      || return 1
    echo "$label: $line"
  done
}
