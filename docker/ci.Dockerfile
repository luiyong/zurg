# syntax=docker/dockerfile:1
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        git \
        curl \
        sudo \
        python3 \
        python3-pip \
        libprotobuf-dev \
        protobuf-compiler \
        protobuf-compiler-grpc \
        libgrpc++-dev \
        libpcap-dev \
        libssl-dev \
        libfmt-dev \
        libgtest-dev \
        libspdlog-dev \
        gcovr \
    && rm -rf /var/lib/apt/lists/*

RUN cmake -S /usr/src/googletest -B /tmp/googletest-build \
    && cmake --build /tmp/googletest-build \
    && cmake --install /tmp/googletest-build \
    && rm -rf /tmp/googletest-build

ENV PATH="/usr/local/bin:${PATH}"

WORKDIR /workspace
