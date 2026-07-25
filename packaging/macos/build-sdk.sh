#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 3 ]]; then
    echo "usage: $0 <version> <commit> <output-directory>" >&2
    exit 2
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "macOS SDKs must be built natively on macOS" >&2
    exit 2
fi

version="$1"
commit="$2"
output_dir="$3"
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_root="$(mktemp -d "${TMPDIR:-/tmp}/rl-macos-build.XXXXXX")"
trap 'rm -rf "${build_root}"' EXIT

case "$(uname -m)" in
    arm64)
        release_arch="aarch64"
        ;;
    x86_64)
        release_arch="amd64"
        ;;
    *)
        echo "unsupported macOS architecture: $(uname -m)" >&2
        exit 2
        ;;
esac

cmake -S "${source_dir}" -B "${build_root}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${build_root}/stage" \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    -DRCLIENT_BUILD_TESTS=ON \
    -DRCLIENT_BUNDLE_OPENSSL=ON \
    -DRCLIENT_RELOCATABLE_PKGCONFIG=ON \
    -DRCLIENT_VERSION="${version}" \
    -DRCLIENT_WARNINGS_AS_ERRORS=ON
cmake --build "${build_root}/build" --parallel
ctest --test-dir "${build_root}/build" --output-on-failure
cmake --install "${build_root}/build" --config Release

openssl_root="${OPENSSL_ROOT_DIR:-$(brew --prefix openssl@3)}"
openssl_root="$(cd "${openssl_root}" && pwd -P)"
openssl_license="${openssl_root}/LICENSE.txt"
if [[ ! -f "${openssl_license}" ]]; then
    echo "OpenSSL license not found at ${openssl_license}" >&2
    exit 1
fi
openssl_version="$("${openssl_root}/bin/openssl" version | awk '{print $2}')"
if [[ ! "${openssl_version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "could not determine the bundled OpenSSL version" >&2
    exit 1
fi
mkdir -p \
    "${build_root}/stage/share/doc/rl-c-client/third-party" \
    "${build_root}/stage/share/rl-c-client"
cp "${openssl_license}" \
    "${build_root}/stage/share/doc/rl-c-client/third-party/OpenSSL-LICENSE.txt"
python3 "${source_dir}/tools/write_dependency_sbom.py" \
    --output "${build_root}/stage/share/rl-c-client/dependencies.spdx.json" \
    --project-version "${version}" \
    --openssl-version "${openssl_version}"

library="${build_root}/stage/lib/librclient.${version}.dylib"
test -f "${library}"
file "${library}" | grep -F "$(uname -m)"
otool -D "${library}" |
    grep -F "@rpath/librclient.${version%%.*}.dylib"
if otool -L "${library}" | grep -Ei "homebrew|libcrypto"; then
    echo "macOS SDK dylib has a non-system crypto load dependency" >&2
    exit 1
fi

python3 "${source_dir}/tools/package_sdk.py" \
    --stage "${build_root}/stage" \
    --output "${output_dir}" \
    --version "${version}" \
    --commit "${commit}" \
    --platform macos \
    --architecture "${release_arch}"
