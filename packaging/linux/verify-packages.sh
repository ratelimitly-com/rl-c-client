#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 3 ]]; then
    echo "usage: $0 <profile> <package-directory> <consumer-source>" >&2
    exit 2
fi

profile="$1"
package_dir="$2"
consumer_source="$3"

case "${profile}" in
    ubuntu24.04|debian13)
        mapfile -t packages < <(
            find "${package_dir}" -maxdepth 1 -name "*.deb" -print
        )
        test "${#packages[@]}" -eq 2
        for package in "${packages[@]}"; do
            package_arch="$(dpkg-deb --field "${package}" Architecture)"
            test "${package_arch}" = "$(dpkg --print-architecture)"
        done
        apt-get update
        apt-get install -y "${packages[@]}"
        runtime_name="$(
            dpkg-deb --field \
                "$(find "${package_dir}" -name "*-runtime.deb")" Package
        )"
        development_name="$(
            dpkg-deb --field \
                "$(find "${package_dir}" -name "*-development.deb")" Package
        )"
        ;;
    fedora44)
        mapfile -t packages < <(
            find "${package_dir}" -maxdepth 1 -name "*.rpm" -print
        )
        test "${#packages[@]}" -eq 2
        for package in "${packages[@]}"; do
            package_arch="$(rpm -qp --qf '%{ARCH}' "${package}")"
            test "${package_arch}" = "$(uname -m)"
        done
        dnf install -y "${packages[@]}"
        runtime_name="$(
            rpm -qp --qf '%{NAME}' \
                "$(find "${package_dir}" -name "*-runtime.rpm")"
        )"
        development_name="$(
            rpm -qp --qf '%{NAME}' \
                "$(find "${package_dir}" -name "*-development.rpm")"
        )"
        ;;
    *)
        echo "unsupported Linux package profile: ${profile}" >&2
        exit 2
        ;;
esac

ldconfig
shared_library="$(ldconfig -p | awk '/librclient.so/{print $NF; exit}')"
test -n "${shared_library}"
readelf -d "${shared_library}" | grep -F "(SONAME)"
ldd "${shared_library}" | grep -F "libcrypto"
if ldd "${shared_library}" | grep -F "not found"; then
    echo "packaged shared library has an unresolved dependency" >&2
    exit 1
fi

pkg-config --modversion rclient | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'
cc "${consumer_source}/main.c" \
    $(pkg-config --cflags --libs rclient) \
    -o /tmp/rclient-pkg-config-consumer
/tmp/rclient-pkg-config-consumer

cmake -S "${consumer_source}" -B /tmp/rclient-cmake-consumer \
    -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/rclient-cmake-consumer --parallel
/tmp/rclient-cmake-consumer/rclient-installed-consumer

case "${profile}" in
    ubuntu24.04|debian13)
        apt-get remove -y "${development_name}" "${runtime_name}"
        ;;
    fedora44)
        dnf remove -y "${development_name}" "${runtime_name}"
        ;;
esac

test ! -e /usr/include/r_client.h
test -z "$(find /usr/lib /usr/lib64 -name "librclient.so*" -print 2>/dev/null)"
