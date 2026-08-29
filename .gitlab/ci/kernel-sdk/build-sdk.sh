#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

usage() {
    cat <<'EOF'
Usage:
  build-sdk.sh <series-or-version>
  build-sdk.sh --all
  build-sdk.sh --force <series-or-version>

Examples:
  sudo ./.gitlab/ci/kernel-sdk/build-sdk.sh 6.6
  sudo ./.gitlab/ci/kernel-sdk/build-sdk.sh 4.9
  sudo ./.gitlab/ci/kernel-sdk/build-sdk.sh --all

Environment:
  SDK_ROOT=/srv/esp-hosted-ci/kernel-sdk
  SRC_ROOT=/srv/esp-hosted-ci/kernel-src
  SDK_JOBS=<parallel kernel build jobs; default: nproc>
EOF
}

[[ ${EUID:-$(id -u)} -eq 0 ]] || die "Run as root; SDK trees are maintained under /srv"

FORCE=0
TARGET=""

case "${1:-}" in
    --all)
        TARGET="--all"
        ;;
    --force)
        FORCE=1
        TARGET="${2:-}"
        [[ -n "$TARGET" ]] || { usage; exit 2; }
        ;;
    -h|--help|"")
        usage
        exit 0
        ;;
    *)
        TARGET="$1"
        ;;
esac

mkdir -p "$SDK_ROOT" "$SRC_ROOT"

build_one() {
    local requested="$1"
    lookup_kernel "$requested"

    local version="$KERNEL_VERSION"
    local series="$KERNEL_SERIES"
    local image="$KERNEL_CI_IMAGE"
    local dest="${SDK_ROOT}/${version}"
    local alias="${SDK_ROOT}/${series}"
    local tarball="${SRC_ROOT}/linux-${version}.tar.xz"
    local tmp="${SDK_ROOT}/.${version}.building"
    local url
    url="$(kernel_url "$version")"
    local jobs="${SDK_JOBS:-$(nproc)}"

    echo "==> Kernel series : $series"
    echo "==> Kernel version: $version"
    echo "==> Builder image : $image"
    echo "==> SDK directory : $dest"

    docker image inspect "$image" >/dev/null 2>&1 ||
        die "Missing Docker image '$image'. Run host/setup-host.sh first."

    if [[ -f "${dest}/.esp-hosted-sdk-ready" && "$FORCE" -eq 0 ]]; then
        echo "==> SDK already ready; refreshing alias only"
        ln -sfn "$version" "$alias"
        return 0
    fi

    if [[ -e "$dest" && "$FORCE" -eq 0 ]]; then
        die "$dest already exists but is not marked ready. Use adopt-sdk.sh $series if it is a valid manually-built SDK, or --force to rebuild."
    fi

    if [[ ! -s "$tarball" ]]; then
        echo "==> Downloading $url"
        docker run --rm \
            -v "${SRC_ROOT}:/src" \
            "$image" \
            bash -lc "set -euo pipefail; \
                base='${url%/*}'; \
                curl -fL --retry 4 --retry-delay 2 '$url' -o '/src/linux-${version}.tar.xz.tmp'; \
                curl -fL --retry 4 --retry-delay 2 \"\${base}/sha256sums.asc\" -o '/src/sha256sums-${version}.asc'; \
                xz -t '/src/linux-${version}.tar.xz.tmp'; \
                expected=\$(grep -E '^[0-9a-fA-F]{64}[[:space:]]+linux-${version}\\.tar\\.xz$' '/src/sha256sums-${version}.asc' | head -1); \
                test -n \"\$expected\"; \
                cd /src; \
                echo \"\$expected\" | sed 's/linux-${version}\\.tar\\.xz/linux-${version}.tar.xz.tmp/' | sha256sum -c -; \
                mv '/src/linux-${version}.tar.xz.tmp' '/src/linux-${version}.tar.xz'"
    else
        echo "==> Reusing $tarball"
    fi

    rm -rf "$tmp"
    mkdir -p "$tmp"

    echo "==> Extracting Linux $version"
    docker run --rm \
        -v "${SRC_ROOT}:/src:ro" \
        -v "${tmp}:/kernel" \
        "$image" \
        bash -lc "set -euo pipefail; tar -xf '/src/linux-${version}.tar.xz' -C /kernel --strip-components=1"

    echo "==> Preparing/building SDK with ${jobs} jobs"
    docker run --rm \
        -e SDK_SERIES="$series" \
        -e SDK_VERSION="$version" \
        -e SDK_IMAGE="$image" \
        -e SDK_JOBS="$jobs" \
        -v "${tmp}:/kernel" \
        "$image" \
        bash -lc '
            set -euo pipefail
            cd /kernel

            make ARCH=x86 mrproper
            make ARCH=x86 x86_64_defconfig

            scripts/config --enable MODULES
            scripts/config --module CFG80211
            scripts/config --enable MMC
            scripts/config --enable SPI
            scripts/config --module BT

            # Keep SDK builds deterministic/lightweight. These options do not
            # affect the API compatibility we are testing.
            scripts/config --disable DEBUG_INFO || true
            scripts/config --disable DEBUG_INFO_BTF || true
            scripts/config --disable WERROR || true

            if scripts/config --help 2>&1 | grep -q -- "--set-str"; then
                if grep -q "CONFIG_SYSTEM_TRUSTED_KEYS" .config; then
                    scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
                fi
                if grep -q "CONFIG_SYSTEM_REVOCATION_KEYS" .config; then
                    scripts/config --set-str SYSTEM_REVOCATION_KEYS ""
                fi
            fi

            make ARCH=x86 olddefconfig

            echo "==> Effective required config"
            grep -E "CONFIG_(MODULES|CFG80211|MMC|SPI|BT)=" .config

            # Full vmlinux + modules is intentional: Module.symvers must
            # include exports from built-ins and module subsystems such as
            # cfg80211/Bluetooth for external-module MODPOST.
            make ARCH=x86 -j"${SDK_JOBS}" vmlinux modules

            test -s Module.symvers
            test -s .config

            make -s ARCH=x86 kernelrelease > .esp-hosted-kernelrelease
            gcc -dumpfullversion > .esp-hosted-gcc-version
            gcc -dumpversion | cut -d. -f1 > .esp-hosted-gcc-major
            printf "%s\n" "$SDK_SERIES" > .esp-hosted-series
            printf "%s\n" "$SDK_VERSION" > .esp-hosted-version
            printf "%s\n" "$SDK_IMAGE" > .esp-hosted-ci-image
            date -u +"%Y-%m-%dT%H:%M:%SZ" > .esp-hosted-built-at
            touch .esp-hosted-sdk-ready
        '

    rm -rf "$dest"
    mv "$tmp" "$dest"
    ln -sfn "$version" "$alias"

    echo "==> READY: $alias -> $version"
    echo "    release : $(cat "${dest}/.esp-hosted-kernelrelease")"
    echo "    gcc     : $(cat "${dest}/.esp-hosted-gcc-version")"
    echo "    symvers : $(du -h "${dest}/Module.symvers" | awk '{print $1}')"
}

if [[ "$TARGET" == "--all" ]]; then
    while IFS= read -r series; do
        build_one "$series"
    done < <(all_series)
else
    build_one "$TARGET"
fi
