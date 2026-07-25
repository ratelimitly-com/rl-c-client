#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image_name="rl-c-client-test-deb:debian13"

docker build \
    --file "${repo_root}/tests/fixtures/debian-package.Dockerfile" \
    --tag "${image_name}" \
    "${repo_root}"

docker run --rm \
    --mount "type=bind,source=${repo_root},target=/src,readonly" \
    "${image_name}" bash -euxo pipefail -c '
cp -a /src /work
cmake -S /work -B /work/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DRCLIENT_PACKAGE_FORMAT=debian13 \
    -DRCLIENT_VERSION=1.2.3
cmake --build /work/build --parallel
cpack --config /work/build/CPackConfig.cmake -G DEB \
    -B /work/packages

mapfile -t packages < <(find /work/packages -maxdepth 1 -name "*.deb" -print)
test "${#packages[@]}" -eq 2
runtime_package="$(
    find /work/packages -maxdepth 1 -name "librclient1_*.deb"
)"
development_package="$(
    find /work/packages -maxdepth 1 -name "librclient-dev_*.deb"
)"
test -n "${runtime_package}"
test -n "${development_package}"

test "$(dpkg-deb --field "${runtime_package}" Package)" = librclient1
test "$(dpkg-deb --field "${development_package}" Package)" = librclient-dev
test "$(dpkg-deb --field "${runtime_package}" Version)" = 1.2.3-1
test "$(dpkg-deb --field "${development_package}" Version)" = 1.2.3-1
test "$(dpkg-deb --field "${runtime_package}" Architecture)" = \
    "$(dpkg --print-architecture)"
test "$(dpkg-deb --field "${development_package}" Architecture)" = \
    "$(dpkg --print-architecture)"
dpkg-deb --field "${runtime_package}" Depends | grep -Eq "libssl3"
dpkg-deb --field "${development_package}" Depends |
    grep -F "librclient1 (= 1.2.3-1)"

dpkg-deb --contents "${runtime_package}" |
    grep -E "/librclient\\.so\\.1 -> librclient\\.so\\.1\\.2\\.3$"
dpkg-deb --contents "${runtime_package}" |
    grep -F "usr/share/doc/rl-c-client/LICENSE"
dpkg-deb --contents "${development_package}" |
    grep -F "usr/include/r_client.h"
dpkg-deb --contents "${development_package}" |
    grep -E "/librclient\\.a$"
dpkg-deb --contents "${development_package}" |
    grep -F "/pkgconfig/rclient.pc"
dpkg-deb --contents "${development_package}" |
    grep -F "/cmake/rclient/rclient-config.cmake"
'

echo "test_deb_packages: PASS"
