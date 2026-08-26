FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG IDF_TAG=v5.5.1
ARG IDF_COMMIT=fcae32885b0296b32044cb99ecbdc50d98dddb83

ENV IDF_PATH=/opt/esp/idf
ENV IDF_TOOLS_PATH=/opt/esp/.espressif
ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    wget \
    flex \
    bison \
    gperf \
    python3 \
    python3-pip \
    python3-venv \
    cmake \
    ninja-build \
    ccache \
    libffi-dev \
    libssl-dev \
    dfu-util \
    libusb-1.0-0 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/esp && \
    git clone --branch "${IDF_TAG}" --depth 100 \
        https://github.com/espressif/esp-idf.git "${IDF_PATH}" && \
    cd "${IDF_PATH}" && \
    git checkout -f "${IDF_COMMIT}"

# ESP-Hosted requires this patch against its pinned IDF.
COPY esp/esp_driver/lib/rom.patch /tmp/rom.patch

RUN cd "${IDF_PATH}" && \
    git apply /tmp/rom.patch && \
    git submodule update --init --depth 1 --recursive && \
    ./install.sh && \
    rm -rf "${IDF_TOOLS_PATH}/dist/"* && \
    rm -f /tmp/rom.patch

LABEL esp.hosted.idf.tag="v5.5.1"
LABEL esp.hosted.idf.commit="fcae32885b0296b32044cb99ecbdc50d98dddb83"

CMD ["/bin/bash"]
