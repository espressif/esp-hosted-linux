#!/usr/bin/env bash
set -euo pipefail

MODE="${1:?usage: linux-quality.sh <warnings|sparse|clang> [kernel-version]}"
KERNEL_VERSION="${2:-6.12.105}"
KERNEL="${KERNEL:-/kernel-sdk/${KERNEL_VERSION}}"
HOST_DIR="${CI_PROJECT_DIR:-$(pwd)}/host"
OUT="${CI_PROJECT_DIR:-$(pwd)}/build-artifacts/linux-quality/${MODE}"
mkdir -p "${OUT}"

test -f "${KERNEL}/Makefile"
test -s "${KERNEL}/.config"
test -s "${KERNEL}/Module.symvers"
[[ "$(make -s -C "${KERNEL}" kernelversion)" == "${KERNEL_VERSION}" ]]

build_one() {
    local transport="$1"
    shift
    local log="${OUT}/${transport}.log"

    make -C "${HOST_DIR}" KERNEL="${KERNEL}" clean >/dev/null || true

    set +e
    make -C "${HOST_DIR}" \
        KERNEL="${KERNEL}" \
        target="${transport}" \
        "$@" 2>&1 | tee "${log}"
    rc=${PIPESTATUS[0]}
    set -e

    (( rc == 0 )) || {
        echo "ERROR: ${MODE} build failed for ${transport}" >&2
        exit "${rc}"
    }

    test -s "${HOST_DIR}/esp32_${transport}.ko"
}

fail_on_host_warning() {
    local log="$1"
    local label="$2"

    # Source warnings are hard failures: CI starts at zero warning debt.
    if grep -E '(/host/|^host/).*\.(c|h):[0-9]+(:[0-9]+)?: warning:' "${log}"; then
        echo "ERROR: ${label} warning in ESP-Hosted host source" >&2
        exit 1
    fi

    # A missing include directory is a build-system error even though GCC
    # reports it without a host/foo.c source prefix.
    if grep -F -- '[-Wmissing-include-dirs]' "${log}"; then
        echo "ERROR: invalid include directory in Linux host build" >&2
        exit 1
    fi
}

case "${MODE}" in
    warnings)
        for transport in sdio spi; do
            build_one "${transport}" CC="ccache gcc" W=1
            fail_on_host_warning "${OUT}/${transport}.log" "W=1"
        done
        ;;

    sparse)
        command -v sparse >/dev/null

        for transport in sdio spi; do
            build_one "${transport}" CC="ccache gcc" C=2 CHECK=sparse
            fail_on_host_warning "${OUT}/${transport}.log" "sparse"
        done
        ;;

    clang)
        command -v clang >/dev/null
        command -v ld.lld >/dev/null

        echo "=== Re-preparing kernel SDK for Clang ==="

        # The immutable kernel SDK was prepared with GCC. Kconfig records
        # compiler-specific capabilities, so using that state directly with
        # Clang enables GCC-only flags such as -fconserve-stack.
        #
        # The CI container is disposable. Re-prepare its private writable
        # kernel tree with Clang, while retaining the SDK's Module.symvers.
        saved_symvers="$(mktemp)"
        cp "${KERNEL}/Module.symvers" "${saved_symvers}"

        make -C "${KERNEL}" CC=clang LD=ld.lld olddefconfig
        make -C "${KERNEL}" CC=clang LD=ld.lld prepare modules_prepare

        # modules_prepare does not generate the complete symbol-version
        # database required by CONFIG_MODVERSIONS kernels.
        cp "${saved_symvers}" "${KERNEL}/Module.symvers"
        rm -f "${saved_symvers}"

        grep -q '^CONFIG_CC_IS_CLANG=y' "${KERNEL}/.config" || {
            echo "ERROR: kernel SDK was not re-prepared for Clang" >&2
            grep -E '^CONFIG_CC_IS_(CLANG|GCC)=' "${KERNEL}/.config" || true
            exit 1
        }

        clang --version | head -1

        for transport in sdio spi; do
            build_one "${transport}" \
                CC="ccache clang" \
                LD=ld.lld
            fail_on_host_warning "${OUT}/${transport}.log" "Clang"
        done
        ;;

    *)
        echo "ERROR: unknown mode: ${MODE}" >&2
        exit 2
        ;;
esac

make -C "${HOST_DIR}" KERNEL="${KERNEL}" clean >/dev/null || true

if command -v ccache >/dev/null 2>&1; then
    ccache -s || true
fi

echo "=== PASS: Linux quality ${MODE} ==="
