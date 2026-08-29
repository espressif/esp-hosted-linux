ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    bc \
    bison \
    flex \
    libssl-dev \
    libelf-dev \
    kmod \
    file \
    curl \
    ca-certificates \
    xz-utils \
    perl \
    python3 \
    git \
    openssl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
