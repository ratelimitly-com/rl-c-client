FROM debian:13@sha256:fac46bff2e02f51425b6e33b0e1169f55dfb053d83511ca28aa50c09fd5ed7a4

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        dpkg-dev \
        libssl-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*
