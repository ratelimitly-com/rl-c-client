#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-embed-direct.XXXXXX")"
trap 'rm -rf "${tmp_dir}"' EXIT

export SOURCE_DATE_EPOCH=1700000000
python3 "${repo_root}/tools/package_source.py" \
    --source "${repo_root}" \
    --output "${tmp_dir}" \
    --version 1.2.3 \
    --commit 0123456789abcdef0123456789abcdef01234567
tar -xzf "${tmp_dir}/rl-c-client-v1.2.3-source.tar.gz" \
    -C "${tmp_dir}"

mkdir -p "${tmp_dir}/consumer"
cat >"${tmp_dir}/consumer/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(rclient-embed-direct LANGUAGES C)

include(../rl-c-client-1.2.3/cmake/rclient-embed.cmake)

add_executable(embed-direct main.c)
target_sources(embed-direct PRIVATE ${RCLIENT_EMBED_SOURCES})
target_include_directories(embed-direct PRIVATE ${RCLIENT_EMBED_INCLUDE_DIRS})
target_link_libraries(embed-direct PRIVATE ${RCLIENT_EMBED_LIBRARIES})
target_compile_features(embed-direct PRIVATE c_std_11)
EOF
cat >"${tmp_dir}/consumer/main.c" <<'EOF'
#include <r_client_runtime.h>

int main(void)
{
    return r_runtime_status_name(0) == 0;
}
EOF

cmake -S "${tmp_dir}/consumer" -B "${tmp_dir}/build" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${tmp_dir}/build" --parallel
"${tmp_dir}/build/embed-direct"

echo "test_source_embed_direct: PASS"
