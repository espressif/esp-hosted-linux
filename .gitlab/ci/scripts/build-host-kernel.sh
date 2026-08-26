#!/usr/bin/env bash
set -euo pipefail

KERNEL_SERIES="${1:?Usage: build-host-kernel.sh <kernel-series>}"
HOST_DRIVER_DIR="${HOST_DRIVER_DIR:-host}"
KERNEL_SDK_ROOT="${KERNEL_SDK_ROOT:-/kernel-sdk}"
HOST_BUILD_JOBS="${HOST_BUILD_JOBS:-2}"
PROJECT_ROOT="${CI_PROJECT_DIR:-$(pwd)}"
ARTIFACT_DIR="${PROJECT_ROOT}/build-artifacts/host/${KERNEL_SERIES}"

alias_path="${KERNEL_SDK_ROOT}/${KERNEL_SERIES}"
[[ -e "$alias_path" ]] || { echo "ERROR: Missing SDK alias $alias_path" >&2; exit 1; }
kernel_dir="$(readlink -f "$alias_path")"

for f in .esp-hosted-sdk-ready .esp-hosted-version .esp-hosted-kernelrelease .esp-hosted-gcc-major Module.symvers .config; do
    if [[ "$f" == ".esp-hosted-sdk-ready" ]]; then
        [[ -e "${kernel_dir}/${f}" ]] || { echo "ERROR: invalid SDK; missing ${kernel_dir}/${f}" >&2; exit 1; }
    else
        [[ -s "${kernel_dir}/${f}" ]] || { echo "ERROR: invalid SDK; missing ${kernel_dir}/${f}" >&2; exit 1; }
    fi
done

expected_gcc="$(cat "${kernel_dir}/.esp-hosted-gcc-major")"
actual_gcc="$(gcc -dumpversion | cut -d. -f1)"
[[ "$actual_gcc" == "$expected_gcc" ]] || {
    echo "ERROR: SDK GCC major=${expected_gcc}, CI image GCC major=${actual_gcc}" >&2
    exit 1
}

version="$(cat "${kernel_dir}/.esp-hosted-version")"
release="$(cat "${kernel_dir}/.esp-hosted-kernelrelease")"

printf '%s\n' \
    "=== ESP-Hosted Linux compatibility build ===" \
    "series       : ${KERNEL_SERIES}" \
    "version      : ${version}" \
    "release      : ${release}" \
    "kernel SDK   : ${kernel_dir}" \
    "driver dir   : ${HOST_DRIVER_DIR}" \
    "compiler     : $(gcc --version | head -1)" \
    "parallelism  : ${HOST_BUILD_JOBS}"

mkdir -p "$ARTIFACT_DIR"
cd "${PROJECT_ROOT}/${HOST_DRIVER_DIR}"

build_transport() {
    local transport="$1"
    local ko="esp32_${transport}.ko"

    echo "=== Linux ${version} / ${transport^^} ==="
    make -j"${HOST_BUILD_JOBS}" KERNEL="${kernel_dir}" ARCH=x86 target="${transport}"
    test -s "$ko"

    local vermagic
    vermagic="$(modinfo -F vermagic "$ko")"
    echo "vermagic: $vermagic"
    grep -Fq "$release" <<<"$vermagic" || {
        echo "ERROR: $ko vermagic does not contain $release" >&2
        exit 1
    }
    cp "$ko" "${ARTIFACT_DIR}/"
}

build_transport sdio
build_transport spi

{
    echo "series=${KERNEL_SERIES}"
    echo "version=${version}"
    echo "release=${release}"
    echo "sdk=${kernel_dir}"
    echo "compiler=$(gcc --version | head -1)"
    echo "sdio_vermagic=$(modinfo -F vermagic "${ARTIFACT_DIR}/esp32_sdio.ko")"
    echo "spi_vermagic=$(modinfo -F vermagic "${ARTIFACT_DIR}/esp32_spi.ko")"
    echo "sdio_sha256=$(sha256sum "${ARTIFACT_DIR}/esp32_sdio.ko" | awk '{print $1}')"
    echo "spi_sha256=$(sha256sum "${ARTIFACT_DIR}/esp32_spi.ko" | awk '{print $1}')"
} > "${ARTIFACT_DIR}/build-info.txt"

cat "${ARTIFACT_DIR}/build-info.txt"
echo "=== PASS: Linux ${version}, SDIO + SPI ==="
