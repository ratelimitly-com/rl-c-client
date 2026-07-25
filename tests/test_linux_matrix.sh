#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-linux-matrix.XXXXXX")"
trap 'rm -rf "${tmp_dir}"' EXIT

profiles=(
    "ubuntu24.04|rl-c-client-test-linux:ubuntu24.04|tests/fixtures/ubuntu-package.Dockerfile"
    "debian13|rl-c-client-test-deb:debian13|tests/fixtures/debian-package.Dockerfile"
    "fedora44|rl-c-client-test-rpm:fedora44|tests/fixtures/fedora-package.Dockerfile"
)

for entry in "${profiles[@]}"; do
    IFS="|" read -r profile image dockerfile <<<"${entry}"
    output_dir="${tmp_dir}/${profile}"
    mkdir -p "${output_dir}"

    docker build \
        --file "${repo_root}/${dockerfile}" \
        --tag "${image}" \
        "${repo_root}"
    docker run --rm \
        --mount "type=bind,source=${repo_root},target=/src,readonly" \
        --mount "type=bind,source=${output_dir},target=/out" \
        "${image}" \
        /src/packaging/linux/build-packages.sh \
        "${profile}" 1.2.3 /out

    test "$(find "${output_dir}" -type f | wc -l | tr -d " ")" -eq 2
    docker run --rm \
        --mount "type=bind,source=${output_dir},target=/packages,readonly" \
        --mount "type=bind,source=${repo_root}/packaging/linux/verify-packages.sh,target=/verify-packages.sh,readonly" \
        --mount "type=bind,source=${repo_root}/tests/fixtures/installed_consumer,target=/consumer,readonly" \
        "${image}" \
        /verify-packages.sh "${profile}" /packages /consumer
done

echo "test_linux_matrix: PASS"
