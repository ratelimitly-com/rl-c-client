FROM fedora:44

RUN dnf install -y \
        cmake \
        findutils \
        gcc \
        gcc-c++ \
        ninja-build \
        openssl-devel \
        rpm-build \
    && dnf clean all
