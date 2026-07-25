#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-msvc-runtime.XXXXXX")"
trap 'rm -rf "${tmp_dir}"' EXIT

cat >"${tmp_dir}/empty.c" <<'EOF'
int fixture(void)
{
    return 0;
}
EOF
cat >"${tmp_dir}/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
project(rclient-msvc-runtime-policy LANGUAGES C)

set(MSVC TRUE)
set(RCLIENT_USE_STATIC_MSVC_RUNTIME ON)
include("${repo_root}/cmake/rclient-msvc-runtime.cmake")

add_library(runtime-fixture STATIC empty.c)
rclient_configure_msvc_runtime(runtime-fixture)
get_target_property(actual_runtime runtime-fixture MSVC_RUNTIME_LIBRARY)
set(expected_runtime "MultiThreaded\\\$<\\\$<CONFIG:Debug>:Debug>")
if(NOT actual_runtime STREQUAL expected_runtime)
    message(FATAL_ERROR
        "expected static MSVC runtime '\${expected_runtime}', "
        "got '\${actual_runtime}'")
endif()
EOF

cmake -S "${tmp_dir}" -B "${tmp_dir}/build"

if rg -n --glob "CMakeLists.txt" --glob "*.cmake" \
    '(^|[ ;"])/MDd?([ ;"]|$)' "${repo_root}"; then
    echo "release build metadata contains a dynamic MSVC runtime flag" >&2
    exit 1
fi

echo "test_msvc_runtime: PASS"
