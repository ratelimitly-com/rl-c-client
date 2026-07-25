#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 1 ]]; then
    echo "usage: $0 <output-prefix>" >&2
    exit 2
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "macOS OpenSSL must be built natively on macOS" >&2
    exit 2
fi

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
config="${source_dir}/manifest/macos-release.json"
output_prefix="$1"
build_root="$(mktemp -d "${TMPDIR:-/tmp}/rl-openssl-build.XXXXXX")"
trap 'rm -rf "${build_root}"' EXIT

read_config() {
    python3 - "${config}" "$1" <<'PY'
import json
from pathlib import Path
import sys

value = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
for component in sys.argv[2].split("."):
    value = value[component]
print(value)
PY
}

deployment_target="$(read_config deployment_target)"
openssl_version="$(read_config openssl.version)"
openssl_url="$(read_config openssl.url)"
openssl_sha256="$(read_config openssl.sha256)"
if [[ ! "${deployment_target}" =~ ^[0-9]+\.[0-9]+$ ]]; then
    echo "invalid macOS deployment target" >&2
    exit 1
fi
if [[ ! "${openssl_version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "invalid pinned OpenSSL version" >&2
    exit 1
fi
if [[ ! "${openssl_sha256}" =~ ^[0-9a-f]{64}$ ]]; then
    echo "invalid pinned OpenSSL SHA-256" >&2
    exit 1
fi

case "$(uname -m)" in
    arm64)
        openssl_target="darwin64-arm64-cc"
        ;;
    x86_64)
        openssl_target="darwin64-x86_64-cc"
        ;;
    *)
        echo "unsupported macOS architecture: $(uname -m)" >&2
        exit 2
        ;;
esac

archive="${build_root}/openssl-${openssl_version}.tar.gz"
curl --fail --location --proto '=https' --tlsv1.2 \
    "${openssl_url}" \
    --output "${archive}"
actual_sha256="$(shasum -a 256 "${archive}" | awk '{print $1}')"
if [[ "${actual_sha256}" != "${openssl_sha256}" ]]; then
    echo "OpenSSL source SHA-256 does not match the pinned digest" >&2
    exit 1
fi
tar -xzf "${archive}" -C "${build_root}"
openssl_source="${build_root}/openssl-${openssl_version}"
test -d "${openssl_source}"

mkdir -p "${output_prefix}"
output_prefix="$(cd "${output_prefix}" && pwd -P)"
export MACOSX_DEPLOYMENT_TARGET="${deployment_target}"
(
    cd "${openssl_source}"
    ./Configure \
        "${openssl_target}" \
        no-shared \
        no-tests \
        --prefix="${output_prefix}" \
        --openssldir="${output_prefix}/ssl"
    make -s -j"$(sysctl -n hw.ncpu)"
    make -s install_sw
)
cp "${openssl_source}/LICENSE.txt" "${output_prefix}/LICENSE.txt"

test -f "${output_prefix}/lib/libcrypto.a"
test -x "${output_prefix}/bin/openssl"
test "$("${output_prefix}/bin/openssl" version | awk '{print $2}')" = \
    "${openssl_version}"
archive_versions="$(
    otool -l "${output_prefix}/lib/libcrypto.a" |
        awk '
            /LC_BUILD_VERSION|LC_VERSION_MIN_MACOSX/ { found=1; next }
            found && ($1 == "minos" || $1 == "version") {
                print $2
                found=0
            }
        ' |
        LC_ALL=C sort -u
)"
if [[ "${archive_versions}" != "${deployment_target}" ]]; then
    echo "OpenSSL archive deployment targets are not ${deployment_target}:" >&2
    printf '%s\n' "${archive_versions}" >&2
    exit 1
fi
