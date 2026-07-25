#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 3 ]]; then
    echo "usage: $0 <profile> <version> <output-directory>" >&2
    exit 2
fi

profile="$1"
version="$2"
output_dir="$3"
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-linux-build.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "version must be numeric MAJOR.MINOR.PATCH" >&2
    exit 2
fi

case "${profile}" in
    ubuntu24.04|debian13)
        generator="DEB"
        extension="deb"
        native_arch="$(dpkg --print-architecture)"
        runtime_pattern="librclient${version%%.*}_*.deb"
        development_pattern="librclient-dev_*.deb"
        ;;
    fedora44)
        generator="RPM"
        extension="rpm"
        native_arch="$(rpm --eval '%{_arch}')"
        runtime_pattern="rclient-libs-*.rpm"
        development_pattern="rclient-devel-*.rpm"
        ;;
    *)
        echo "unsupported Linux package profile: ${profile}" >&2
        exit 2
        ;;
esac

case "${native_arch}" in
    amd64|x86_64)
        release_arch="amd64"
        ;;
    arm64|aarch64)
        release_arch="aarch64"
        ;;
    *)
        echo "unsupported Linux architecture: ${native_arch}" >&2
        exit 2
        ;;
esac

cmake -S "${source_dir}" -B "${build_dir}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DRCLIENT_BUILD_TESTS=ON \
    -DRCLIENT_PACKAGE_FORMAT="${profile}" \
    -DRCLIENT_VERSION="${version}" \
    -DRCLIENT_WARNINGS_AS_ERRORS=ON
cmake --build "${build_dir}/build" --parallel
ctest --test-dir "${build_dir}/build" --output-on-failure

shared_library="$(
    find "${build_dir}/build" -maxdepth 1 -type f \
        -name "librclient.so.${version}"
)"
test -n "${shared_library}"
readelf -d "${shared_library}" |
    grep -F "(SONAME)" |
    grep -F "[librclient.so.${version%%.*}]"
if readelf -d "${shared_library}" | grep -Eq "\\((RPATH|RUNPATH)\\)"; then
    echo "packaged shared library contains an RPATH or RUNPATH" >&2
    exit 1
fi

cpack --config "${build_dir}/build/CPackConfig.cmake" \
    -G "${generator}" \
    -B "${build_dir}/packages"

mapfile -t runtime_packages < <(
    find "${build_dir}/packages" -maxdepth 1 \
        -name "${runtime_pattern}" -print
)
mapfile -t development_packages < <(
    find "${build_dir}/packages" -maxdepth 1 \
        -name "${development_pattern}" -print
)
test "${#runtime_packages[@]}" -eq 1
test "${#development_packages[@]}" -eq 1

mkdir -p "${output_dir}"
install -m 0644 "${runtime_packages[0]}" \
    "${output_dir}/rl-c-client-v${version}-${profile}-${release_arch}-runtime.${extension}"
install -m 0644 "${development_packages[0]}" \
    "${output_dir}/rl-c-client-v${version}-${profile}-${release_arch}-development.${extension}"
