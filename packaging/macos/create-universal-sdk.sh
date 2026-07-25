#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 5 ]]; then
    echo "usage: $0 <aarch64-sdk> <amd64-sdk> <version> <commit> <output-directory>" >&2
    exit 2
fi
if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "universal macOS SDKs must be assembled on macOS" >&2
    exit 2
fi

aarch64_archive="$1"
amd64_archive="$2"
version="$3"
commit="$4"
output_dir="$5"
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-universal-build.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT

mkdir -p "${work_dir}/aarch64" "${work_dir}/amd64"
tar -xzf "${aarch64_archive}" -C "${work_dir}/aarch64"
tar -xzf "${amd64_archive}" -C "${work_dir}/amd64"
aarch64_root="${work_dir}/aarch64/rl-c-client-${version}-macos-aarch64"
amd64_root="${work_dir}/amd64/rl-c-client-${version}-macos-amd64"
test -d "${aarch64_root}"
test -d "${amd64_root}"

fingerprint_metadata() {
    local root="$1"
    (
        cd "${root}"
        while IFS= read -r path; do
            case "${path}" in
                ./SDK-MANIFEST.json|./lib/librclient*)
                    continue
                    ;;
            esac
            shasum -a 256 "${path}"
        done < <(find . -type f -print | LC_ALL=C sort)
        while IFS= read -r path; do
            printf '%s -> %s\n' "${path}" "$(readlink "${path}")"
        done < <(find . -type l -print | LC_ALL=C sort)
    )
}

fingerprint_metadata "${aarch64_root}" >"${work_dir}/aarch64.metadata"
fingerprint_metadata "${amd64_root}" >"${work_dir}/amd64.metadata"
diff -u "${work_dir}/aarch64.metadata" "${work_dir}/amd64.metadata"

aarch64_library="${aarch64_root}/lib/librclient.${version}.dylib"
amd64_library="${amd64_root}/lib/librclient.${version}.dylib"
test "$(lipo -archs "${aarch64_library}")" = arm64
test "$(lipo -archs "${amd64_library}")" = x86_64
test "$(lipo -archs "${aarch64_root}/lib/librclient.a")" = arm64
test "$(lipo -archs "${amd64_root}/lib/librclient.a")" = x86_64

otool -D "${aarch64_library}" | tail -n 1 >"${work_dir}/aarch64.install-name"
otool -D "${amd64_library}" | tail -n 1 >"${work_dir}/amd64.install-name"
diff -u "${work_dir}/aarch64.install-name" "${work_dir}/amd64.install-name"
nm -gU "${aarch64_library}" |
    awk '$2 ~ /^[TDSB]$/ {print $3}' |
    sort >"${work_dir}/aarch64.symbols"
nm -gU "${amd64_library}" |
    awk '$2 ~ /^[TDSB]$/ {print $3}' |
    sort >"${work_dir}/amd64.symbols"
diff -u "${work_dir}/aarch64.symbols" "${work_dir}/amd64.symbols"

universal_root="${work_dir}/rl-c-client-${version}-macos-universal2"
cp -a "${aarch64_root}" "${universal_root}"
lipo -create "${aarch64_library}" "${amd64_library}" \
    -output "${universal_root}/lib/librclient.${version}.dylib"
lipo -create \
    "${aarch64_root}/lib/librclient.a" \
    "${amd64_root}/lib/librclient.a" \
    -output "${universal_root}/lib/librclient.a"

python3 "${source_dir}/tools/package_sdk.py" \
    --stage "${universal_root}" \
    --output "${output_dir}" \
    --version "${version}" \
    --commit "${commit}" \
    --platform macos \
    --architecture universal2
