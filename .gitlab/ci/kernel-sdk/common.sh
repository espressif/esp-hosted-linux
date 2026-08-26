#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_TABLE="${KERNEL_TABLE:-${SCRIPT_DIR}/kernels.tsv}"
SDK_ROOT="${SDK_ROOT:-/srv/esp-hosted-ci/kernel-sdk}"
SRC_ROOT="${SRC_ROOT:-/srv/esp-hosted-ci/kernel-src}"

die() { echo "ERROR: $*" >&2; exit 1; }

lookup_kernel() {
    local requested="$1" line
    line="$(awk -F '\t' -v q="$requested" '$0 !~ /^#/ && NF >= 4 && ($1 == q || $2 == q) {print $1 "\t" $2 "\t" $3 "\t" $4; exit}' "$KERNEL_TABLE")"
    [[ -n "$line" ]] || die "Unknown kernel '$requested' in $KERNEL_TABLE"
    IFS=$'\t' read -r KERNEL_SERIES KERNEL_VERSION KERNEL_CI_IMAGE KERNEL_STATUS <<<"$line"
    export KERNEL_SERIES KERNEL_VERSION KERNEL_CI_IMAGE KERNEL_STATUS
}

kernel_url() {
    local version="$1" major="${version%%.*}"
    printf 'https://cdn.kernel.org/pub/linux/kernel/v%s.x/linux-%s.tar.xz\n' "$major" "$version"
}

all_series() { awk -F '\t' '$0 !~ /^#/ && NF >= 4 {print $1}' "$KERNEL_TABLE"; }
