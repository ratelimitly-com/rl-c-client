#!/usr/bin/env bash

# Shared trusted-main production profile. The release/default client policy
# remains U=20 ms with one replay; production CI deliberately allows more
# network variance and records how much of the scheduler window was consumed.
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
  local -a all_lines=()

  [[ -r $file ]] || return 1
  mapfile -t all_lines <"$file"
  for line in "${all_lines[@]}"; do
    normalized=${line%$'\r'}
    if [[ $normalized == 'rl-c-client[profile]:'* ]]; then
      profiles+=("$normalized")
    fi
  done

  [[ ${#profiles[@]} -eq $expected_count ]] || return 1
  if [[ $only_profiles == true && ${#all_lines[@]} -ne $expected_count ]]; then
    return 1
  fi
  for line in "${profiles[@]}"; do
    [[ $line =~ ^rl-c-client\[profile\]:\ wait_ms=[0-9]+\ unit_ms=25\ replay_count=3\ round=[0-3]\ phase=(round|final)\ status=0\ response=selected$ ]] \
      || return 1
    echo "$label: $line"
  done
}
