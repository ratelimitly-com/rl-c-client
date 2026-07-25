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
        runtime_package="$(
            find "${package_dir}" -maxdepth 1 -name "*-runtime.deb"
        )"
        development_package="$(
            find "${package_dir}" -maxdepth 1 -name "*-development.deb"
        )"
        test -n "${runtime_package}"
        test -n "${development_package}"
        for package in "${packages[@]}"; do
            package_arch="$(dpkg-deb --field "${package}" Architecture)"
            test "${package_arch}" = "$(dpkg --print-architecture)"
        done
        runtime_name="$(dpkg-deb --field "${runtime_package}" Package)"
        runtime_version="$(dpkg-deb --field "${runtime_package}" Version)"
        development_name="$(
            dpkg-deb --field "${development_package}" Package
        )"
        test "${development_name}" = librclient-dev
        dpkg-deb --field "${runtime_package}" Depends | grep -Eq "libssl3"
        dpkg-deb --field "${development_package}" Depends |
            grep -F "${runtime_name} (= ${runtime_version})"
        runtime_contents="$(dpkg-deb --contents "${runtime_package}")"
        development_contents="$(
            dpkg-deb --contents "${development_package}"
        )"
        grep -F "usr/share/doc/rl-c-client/LICENSE" \
            <<<"${runtime_contents}"
        grep -E "/librclient[.]so[.][0-9]+([.][0-9]+){2}$" \
            <<<"${runtime_contents}"
        if grep -Eq "/include/|/librclient[.]a$|/rclient[.]pc$|/cmake/" \
            <<<"${runtime_contents}"; then
            echo "Debian runtime package contains development files" >&2
            exit 1
        fi
        for pattern in \
            "usr/include/r_client.h" \
            "/librclient.a" \
            "/pkgconfig/rclient.pc" \
            "/cmake/rclient/rclient-config.cmake"; do
            grep -F "${pattern}" <<<"${development_contents}"
        done
        apt-get update
        apt-get install -y "${packages[@]}"
        ;;
    fedora44)
        mapfile -t packages < <(
            find "${package_dir}" -maxdepth 1 -name "*.rpm" -print
        )
        test "${#packages[@]}" -eq 2
        runtime_package="$(
            find "${package_dir}" -maxdepth 1 -name "*-runtime.rpm"
        )"
        development_package="$(
            find "${package_dir}" -maxdepth 1 -name "*-development.rpm"
        )"
        test -n "${runtime_package}"
        test -n "${development_package}"
        for package in "${packages[@]}"; do
            package_arch="$(rpm -qp --qf '%{ARCH}' "${package}")"
            test "${package_arch}" = "$(uname -m)"
        done
        runtime_name="$(rpm -qp --qf '%{NAME}' "${runtime_package}")"
        runtime_version="$(
            rpm -qp --qf '%{VERSION}-%{RELEASE}' "${runtime_package}"
        )"
        development_name="$(
            rpm -qp --qf '%{NAME}' "${development_package}"
        )"
        test "${runtime_name}" = rclient-libs
        test "${development_name}" = rclient-devel
        rpm -qp --requires "${runtime_package}" | grep -F "openssl-libs"
        rpm -qp --requires "${development_package}" |
            grep -F "${runtime_name} = ${runtime_version}"
        runtime_contents="$(rpm -qlp "${runtime_package}")"
        development_contents="$(rpm -qlp "${development_package}")"
        grep -F "/usr/share/doc/rl-c-client/LICENSE" \
            <<<"${runtime_contents}"
        grep -E "/librclient[.]so[.][0-9]+([.][0-9]+){2}$" \
            <<<"${runtime_contents}"
        if grep -Eq "/include/|/librclient[.]a$|/rclient[.]pc$|/cmake/" \
            <<<"${runtime_contents}"; then
            echo "RPM runtime package contains development files" >&2
            exit 1
        fi
        for pattern in \
            "/usr/include/r_client.h" \
            "/librclient.a" \
            "/pkgconfig/rclient.pc" \
            "/cmake/rclient/rclient-config.cmake"; do
            grep -F "${pattern}" <<<"${development_contents}"
        done
        dnf install -y "${packages[@]}"
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
