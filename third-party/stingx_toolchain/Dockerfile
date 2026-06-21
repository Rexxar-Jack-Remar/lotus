FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        bash \
        bison \
        build-essential \
        cmake \
        flex \
        libgmp-dev \
        libppl-dev \
        libz3-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
