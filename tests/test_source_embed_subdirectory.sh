#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-embed-subdir.XXXXXX")"
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
project(rclient-embed-subdirectory LANGUAGES C)

set(RCLIENT_BUILD_TESTS OFF)
set(RCLIENT_BUILD_EXAMPLES OFF)
set(RCLIENT_ENABLE_INSTALL OFF)
add_subdirectory(../rl-c-client-1.2.3 rclient)

add_executable(embed-subdirectory main.c)
target_link_libraries(embed-subdirectory PRIVATE rclient::rclient)
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
"${tmp_dir}/build/embed-subdirectory"

if grep -q '^BUILD_TESTING:' "${tmp_dir}/build/CMakeCache.txt"; then
    echo "rl-c-client polluted its parent with the BUILD_TESTING cache option" >&2
    exit 1
fi

echo "test_source_embed_subdirectory: PASS"
