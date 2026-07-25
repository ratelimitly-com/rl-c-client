#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-shared-only.XXXXXX")"
trap 'rm -rf "${tmp_dir}"' EXIT

cmake -S "${repo_root}" -B "${tmp_dir}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${tmp_dir}/stage" \
    -DRCLIENT_BUILD_STATIC=ON \
    -DRCLIENT_BUILD_SHARED=ON \
    -DRCLIENT_BUNDLE_OPENSSL=ON \
    -DRCLIENT_BUILD_TESTS=OFF \
    -DRCLIENT_VERSION=1.2.3
cmake --build "${tmp_dir}/build" --parallel
cmake --install "${tmp_dir}/build"

cmake -S "${repo_root}/tests/fixtures/shared_only_consumer" \
    -B "${tmp_dir}/consumer" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${tmp_dir}/stage" \
    -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE \
    -DCMAKE_DISABLE_FIND_PACKAGE_Threads=TRUE
cmake --build "${tmp_dir}/consumer" --parallel

case "$(uname -s)" in
    Darwin)
        DYLD_LIBRARY_PATH="${tmp_dir}/stage/lib" \
            "${tmp_dir}/consumer/rclient-shared-only-consumer"
        ;;
    *)
        LD_LIBRARY_PATH="${tmp_dir}/stage/lib" \
            "${tmp_dir}/consumer/rclient-shared-only-consumer"
        ;;
esac

cmake -S "${repo_root}/tests/fixtures/shared_only_consumer" \
    -B "${tmp_dir}/static-consumer" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${tmp_dir}/stage" \
    -DRCLIENT_CONSUMER_STATIC=ON
cmake --build "${tmp_dir}/static-consumer" --parallel
"${tmp_dir}/static-consumer/rclient-shared-only-consumer"

echo "test_shared_only_install: PASS"
