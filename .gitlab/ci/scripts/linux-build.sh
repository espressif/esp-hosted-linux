#!/usr/bin/env bash
set -euo pipefail

KERNEL_VERSION="${1:?usage: linux-build.sh <kernel-version>}"
ARCH="${ARCH:-x86}"
CROSS_COMPILE="${CROSS_COMPILE:-}"
KERNEL="${KERNEL:-/kernel-sdk/${KERNEL_VERSION}}"
HOST_DIR="${CI_PROJECT_DIR:-$(pwd)}/host"
OUT_ROOT="${CI_PROJECT_DIR:-$(pwd)}/build-artifacts/linux/${KERNEL_VERSION}"

if command -v ccache >/dev/null 2>&1; then
    CC_CMD="${CC_CMD:-ccache gcc}"
else
    CC_CMD="${CC_CMD:-gcc}"
fi

echo "=== Linux compatibility build ==="
echo "kernel        : ${KERNEL_VERSION}"
echo "kernel tree   : ${KERNEL}"
echo "arch          : ${ARCH}"
echo "cross compile : ${CROSS_COMPILE:-<none>}"
echo "compiler      : ${CC_CMD}"
echo "compiler info : $(${CC_CMD} --version | head -1)"

test -f "${KERNEL}/Makefile"
test -s "${KERNEL}/.config"
test -s "${KERNEL}/Module.symvers"

ACTUAL_KERNEL="$(make -s -C "${KERNEL}" kernelversion)"
[[ "${ACTUAL_KERNEL}" == "${KERNEL_VERSION}" ]] || {
    echo "ERROR: image kernel mismatch: expected ${KERNEL_VERSION}, got ${ACTUAL_KERNEL}" >&2
    exit 1
}

require_kconfig() {
    local sym="$1"
    if ! grep -Eq "^${sym}=(y|m)$" "${KERNEL}/.config"; then
        echo "ERROR: ${KERNEL_VERSION} SDK missing required ${sym}=y/m" >&2
        exit 1
    fi
}

require_kconfig CONFIG_CFG80211
require_kconfig CONFIG_BT
require_kconfig CONFIG_MMC
require_kconfig CONFIG_SPI

mkdir -p "${OUT_ROOT}/sdio" "${OUT_ROOT}/spi"

build_transport() {
    local transport="$1"
    local module="esp32_${transport}.ko"
    local out="${OUT_ROOT}/${transport}"
    local log="${out}/build.log"

    echo "=== ${KERNEL_VERSION} / ${transport^^} ==="

    make -C "${HOST_DIR}" \
        KERNEL="${KERNEL}" \
        ARCH="${ARCH}" \
        CROSS_COMPILE="${CROSS_COMPILE}" \
        clean >/dev/null || true

    set +e
    make -C "${HOST_DIR}" \
        KERNEL="${KERNEL}" \
        ARCH="${ARCH}" \
        CROSS_COMPILE="${CROSS_COMPILE}" \
        CC="${CC_CMD}" \
        target="${transport}" 2>&1 | tee "${log}"
    rc=${PIPESTATUS[0]}
    set -e
    (( rc == 0 )) || exit "${rc}"

    test -s "${HOST_DIR}/${module}"

    local vermagic
    vermagic="$(modinfo -F vermagic "${HOST_DIR}/${module}")"
    echo "${transport} vermagic: ${vermagic}"
    [[ "${vermagic}" == "${KERNEL_VERSION}"* ]] || {
        echo "ERROR: wrong vermagic for ${module}: ${vermagic}" >&2
        exit 1
    }

    cp "${HOST_DIR}/${module}" "${out}/"
    modinfo "${HOST_DIR}/${module}" > "${out}/modinfo.txt"
    sha256sum "${HOST_DIR}/${module}" > "${out}/sha256.txt"
    stat -c '%n %s bytes' "${HOST_DIR}/${module}" > "${out}/size.txt"
}

build_transport sdio
build_transport spi

make -C "${HOST_DIR}" \
    KERNEL="${KERNEL}" \
    ARCH="${ARCH}" \
    CROSS_COMPILE="${CROSS_COMPILE}" \
    clean >/dev/null || true

{
    echo "kernel=${KERNEL_VERSION}"
    echo "arch=${ARCH}"
    echo "cross_compile=${CROSS_COMPILE}"
    echo "compiler=$(${CC_CMD} --version | head -1)"
    echo "commit=${CI_COMMIT_SHA:-unknown}"
} > "${OUT_ROOT}/build-info.txt"

if command -v ccache >/dev/null 2>&1; then
    echo "=== ccache ==="
    ccache -s || true
fi

echo "=== PASS: Linux ${KERNEL_VERSION} SDIO + SPI ==="
