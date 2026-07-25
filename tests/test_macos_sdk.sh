#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "test_macos_sdk: SKIP (requires macOS)"
    exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/rl-macos-sdk.XXXXXX")"
trap 'rm -rf "${tmp_dir}"' EXIT

export SOURCE_DATE_EPOCH=1700000000
"${repo_root}/packaging/macos/build-sdk.sh" \
    1.2.3 \
    0123456789abcdef0123456789abcdef01234567 \
    "${tmp_dir}"

machine="$(uname -m)"
case "${machine}" in
    arm64)
        release_arch="aarch64"
        ;;
    x86_64)
        release_arch="amd64"
        ;;
    *)
        echo "unsupported macOS test architecture: ${machine}" >&2
        exit 1
        ;;
esac

archive="${tmp_dir}/rl-c-client-v1.2.3-macos-${release_arch}-sdk.tar.gz"
test -f "${archive}"
tar -xzf "${archive}" -C "${tmp_dir}"
sdk_root="${tmp_dir}/rl-c-client-1.2.3-macos-${release_arch}"
library="${sdk_root}/lib/librclient.1.2.3.dylib"
test -s \
    "${sdk_root}/share/doc/rl-c-client/third-party/OpenSSL-LICENSE.txt"
test -s "${sdk_root}/share/rl-c-client/dependencies.spdx.json"

file "${library}" | grep -F "${machine}"
otool -D "${library}" | grep -F "@rpath/librclient.1.dylib"
otool -l "${library}" |
    awk '
        /LC_BUILD_VERSION|LC_VERSION_MIN_MACOSX/ { found=1; next }
        found && ($1 == "minos" || $1 == "version") {
            print $2
            exit
        }
    ' |
    grep -Fx 12.0
if otool -L "${library}" | grep -Ei "homebrew|libcrypto"; then
    echo "macOS SDK dylib has a non-system crypto load dependency" >&2
    exit 1
fi
nm -gU "${library}" |
    awk '$2 ~ /^[TDSB]$/ {print $3}' |
    sed 's/^_//' |
    sort >"${tmp_dir}/symbols"
diff -u "${repo_root}/manifest/public-api.symbols" "${tmp_dir}/symbols"

PKG_CONFIG_PATH="${sdk_root}/lib/pkgconfig" \
    cc "${repo_root}/tests/fixtures/installed_consumer/main.c" \
    $(PKG_CONFIG_PATH="${sdk_root}/lib/pkgconfig" \
        pkg-config --cflags --libs rclient) \
    -o "${tmp_dir}/pkg-config-consumer"
DYLD_LIBRARY_PATH="${sdk_root}/lib" "${tmp_dir}/pkg-config-consumer"

cmake -S "${repo_root}/tests/fixtures/installed_consumer" \
    -B "${tmp_dir}/consumer-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${sdk_root}"
cmake --build "${tmp_dir}/consumer-build" --parallel
DYLD_LIBRARY_PATH="${sdk_root}/lib" \
    "${tmp_dir}/consumer-build/rclient-installed-consumer"

python3 - "${sdk_root}/SDK-MANIFEST.json" "${release_arch}" <<'PY'
import json
from pathlib import Path
import sys

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
assert manifest["architecture"] == sys.argv[2]
assert manifest["platform"] == "macos"
assert manifest["version"] == "1.2.3"
assert manifest["commit"] == "0123456789abcdef0123456789abcdef01234567"
assert manifest["files"] == sorted(
    manifest["files"], key=lambda item: item["path"]
)
paths = {entry["path"] for entry in manifest["files"]}
assert "share/doc/rl-c-client/third-party/OpenSSL-LICENSE.txt" in paths
assert "share/rl-c-client/dependencies.spdx.json" in paths

dependencies = json.loads(
    (
        Path(sys.argv[1]).parent
        / "share/rl-c-client/dependencies.spdx.json"
    ).read_text(encoding="utf-8")
)
packages = {package["name"]: package for package in dependencies["packages"]}
assert packages["OpenSSL"]["licenseDeclared"] == "Apache-2.0"
assert packages["OpenSSL"]["versionInfo"].count(".") == 2
PY

echo "test_macos_sdk: PASS"
