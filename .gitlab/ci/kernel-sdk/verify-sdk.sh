#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

verify_one() {
    lookup_kernel "$1"
    local alias="${SDK_ROOT}/${KERNEL_SERIES}"
    local path

    [[ -L "$alias" || -d "$alias" ]] || die "Missing SDK alias: $alias"
    path="$(readlink -f "$alias")"

    [[ -f "$path/.esp-hosted-sdk-ready" ]] || die "$path is not marked ready"
    [[ -s "$path/Module.symvers" ]] || die "$path/Module.symvers missing/empty"
    [[ -s "$path/.config" ]] || die "$path/.config missing"
    [[ "$(cat "$path/.esp-hosted-version")" == "$KERNEL_VERSION" ]] ||
        die "$KERNEL_SERIES alias points to unexpected version"

    printf '%-6s %-12s %-8s %-22s %s\n' \
        "$KERNEL_SERIES" \
        "$KERNEL_VERSION" \
        "PASS" \
        "$(cat "$path/.esp-hosted-gcc-version")" \
        "$path"
}

case "${1:-}" in
    --all)
        printf '%-6s %-12s %-8s %-22s %s\n' SERIES VERSION STATUS GCC PATH
        while IFS= read -r series; do
            verify_one "$series"
        done < <(all_series)
        ;;
    "")
        echo "Usage: $0 <series-or-version>|--all" >&2
        exit 2
        ;;
    *)
        printf '%-6s %-12s %-8s %-22s %s\n' SERIES VERSION STATUS GCC PATH
        verify_one "$1"
        ;;
esac
