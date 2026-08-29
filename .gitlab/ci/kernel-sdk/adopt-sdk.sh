#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

usage() {
    echo "Usage: sudo $0 <series-or-version>"
    echo "Example: sudo $0 6.6"
}

[[ ${EUID:-$(id -u)} -eq 0 ]] || die "Run as root"
[[ -n "${1:-}" ]] || { usage; exit 2; }

lookup_kernel "$1"

dest="${SDK_ROOT}/${KERNEL_VERSION}"
alias="${SDK_ROOT}/${KERNEL_SERIES}"

[[ -d "$dest" ]] || die "SDK directory not found: $dest"
[[ -s "${dest}/Module.symvers" ]] || die "Missing/empty $dest/Module.symvers"
[[ -s "${dest}/.config" ]] || die "Missing $dest/.config"

docker image inspect "$KERNEL_CI_IMAGE" >/dev/null 2>&1 ||
    die "Missing Docker image '$KERNEL_CI_IMAGE'. Run host/setup-host.sh first."

echo "==> Validating existing SDK: $dest"

docker run --rm \
    -e SDK_SERIES="$KERNEL_SERIES" \
    -e SDK_VERSION="$KERNEL_VERSION" \
    -e SDK_IMAGE="$KERNEL_CI_IMAGE" \
    -v "${dest}:/kernel" \
    "$KERNEL_CI_IMAGE" \
    bash -lc '
        set -euo pipefail
        cd /kernel
        test -s Module.symvers
        test -s .config
        grep -E "CONFIG_(CFG80211|MMC|SPI|BT)=" .config
        make -s ARCH=x86 kernelrelease > .esp-hosted-kernelrelease
        gcc -dumpfullversion > .esp-hosted-gcc-version
        gcc -dumpversion | cut -d. -f1 > .esp-hosted-gcc-major
        printf "%s\n" "$SDK_SERIES" > .esp-hosted-series
        printf "%s\n" "$SDK_VERSION" > .esp-hosted-version
        printf "%s\n" "$SDK_IMAGE" > .esp-hosted-ci-image
        date -u +"%Y-%m-%dT%H:%M:%SZ" > .esp-hosted-built-at
        touch .esp-hosted-sdk-ready
    '

ln -sfn "$KERNEL_VERSION" "$alias"

echo "==> ADOPTED: $alias -> $KERNEL_VERSION"
echo "    release: $(cat "${dest}/.esp-hosted-kernelrelease")"
echo "    gcc    : $(cat "${dest}/.esp-hosted-gcc-version")"
