#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image_name="rl-c-client-test-rpm:fedora44"

docker build \
    --file "${repo_root}/tests/fixtures/fedora-package.Dockerfile" \
    --tag "${image_name}" \
    "${repo_root}"

docker run --rm \
    --mount "type=bind,source=${repo_root},target=/src,readonly" \
    "${image_name}" bash -euxo pipefail -c '
cp -a /src /work
cmake -S /work -B /work/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DRCLIENT_PACKAGE_FORMAT=fedora44 \
    -DRCLIENT_VERSION=1.2.3
cmake --build /work/build --parallel
cpack --config /work/build/CPackConfig.cmake -G RPM \
    -B /work/packages

mapfile -t packages < <(find /work/packages -maxdepth 1 -name "*.rpm" -print)
test "${#packages[@]}" -eq 2
runtime_package="$(
    find /work/packages -maxdepth 1 -name "rclient-libs-*.rpm"
)"
development_package="$(
    find /work/packages -maxdepth 1 -name "rclient-devel-*.rpm"
)"
test -n "${runtime_package}"
test -n "${development_package}"

test "$(rpm -qp --qf "%{NAME}" "${runtime_package}")" = rclient-libs
test "$(rpm -qp --qf "%{NAME}" "${development_package}")" = rclient-devel
test "$(rpm -qp --qf "%{VERSION}-%{RELEASE}" "${runtime_package}")" = \
    1.2.3-1
test "$(rpm -qp --qf "%{ARCH}" "${runtime_package}")" = "$(uname -m)"
rpm -qp --requires "${runtime_package}" | grep -F "openssl-libs"
rpm -qp --requires "${development_package}" |
    grep -F "rclient-libs = 1.2.3-1"

rpm -qlp "${runtime_package}" | grep -E "/librclient\\.so\\.1$"
rpm -qlp "${runtime_package}" |
    grep -F "/usr/share/doc/rl-c-client/LICENSE"
rpm -qlp "${development_package}" | grep -F "/usr/include/r_client.h"
rpm -qlp "${development_package}" | grep -E "/librclient\\.a$"
rpm -qlp "${development_package}" | grep -F "/pkgconfig/rclient.pc"
rpm -qlp "${development_package}" |
    grep -F "/cmake/rclient/rclient-config.cmake"
'

echo "test_rpm_packages: PASS"
