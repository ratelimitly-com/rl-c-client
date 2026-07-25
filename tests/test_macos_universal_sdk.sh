#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "test_macos_universal_sdk: SKIP (requires macOS)"
    exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-macos-universal.XXXXXX")"
trap 'rm -rf "${tmp_dir}"' EXIT

export SOURCE_DATE_EPOCH=1700000000
commit=0123456789abcdef0123456789abcdef01234567
version=1.2.3
cat >"${tmp_dir}/fixture.c" <<'EOF'
int rclient_fixture(void)
{
    return 0;
}
EOF

for arch in arm64 x86_64; do
    case "${arch}" in
        arm64)
            release_arch=aarch64
            ;;
        x86_64)
            release_arch=amd64
            ;;
    esac
    stage="${tmp_dir}/stage-${release_arch}"
    mkdir -p "${stage}/include" "${stage}/lib/cmake/rclient"
    cp "${repo_root}/include/r_client.h" "${stage}/include/"
    cp "${repo_root}/cmake/rclient-config.cmake.in" \
        "${stage}/lib/cmake/rclient/rclient-config.cmake"
    clang -arch "${arch}" -dynamiclib \
        -Wl,-install_name,@rpath/librclient.1.dylib \
        "${tmp_dir}/fixture.c" \
        -o "${stage}/lib/librclient.1.2.3.dylib"
    clang -arch "${arch}" -c "${tmp_dir}/fixture.c" \
        -o "${tmp_dir}/rclient-${release_arch}.o"
    ar rcs "${stage}/lib/librclient.a" \
        "${tmp_dir}/rclient-${release_arch}.o"
    ln -s librclient.1.2.3.dylib "${stage}/lib/librclient.1.dylib"
    ln -s librclient.1.dylib "${stage}/lib/librclient.dylib"
    python3 "${repo_root}/tools/package_sdk.py" \
        --stage "${stage}" \
        --output "${tmp_dir}" \
        --version "${version}" \
        --commit "${commit}" \
        --platform macos \
        --architecture "${release_arch}"
done

"${repo_root}/packaging/macos/create-universal-sdk.sh" \
    "${tmp_dir}/rl-c-client-v${version}-macos-aarch64-sdk.tar.gz" \
    "${tmp_dir}/rl-c-client-v${version}-macos-amd64-sdk.tar.gz" \
    "${version}" \
    "${commit}" \
    "${tmp_dir}/universal"

archive="${tmp_dir}/universal/rl-c-client-v${version}-macos-universal2-sdk.tar.gz"
test -f "${archive}"
tar -xzf "${archive}" -C "${tmp_dir}"
sdk_root="${tmp_dir}/rl-c-client-${version}-macos-universal2"
lipo -archs "${sdk_root}/lib/librclient.1.2.3.dylib" |
    grep -Eq "arm64 x86_64|x86_64 arm64"
lipo -archs "${sdk_root}/lib/librclient.a" |
    grep -Eq "arm64 x86_64|x86_64 arm64"
cmp "${repo_root}/include/r_client.h" "${sdk_root}/include/r_client.h"
python3 - "${sdk_root}/SDK-MANIFEST.json" <<'PY'
import json
from pathlib import Path
import sys

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
assert manifest["architecture"] == "universal2"
assert manifest["platform"] == "macos"
PY

echo "test_macos_universal_sdk: PASS"
