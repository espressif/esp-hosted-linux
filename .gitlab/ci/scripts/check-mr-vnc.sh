#!/usr/bin/env bash
# The MR template always includes "VNC MR:". This script validates that
# field only when the MR updates prebuilt Wi-Fi archives.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=git-base.sh
source "${SCRIPT_DIR}/git-base.sh"

if [[ "${CI_PIPELINE_SOURCE:-}" != "merge_request_event" ]]; then
    echo "Not a merge-request pipeline; skipping."
    exit 0
fi

BASE="$(ci_resolve_diff_base)"
HEAD="${CI_MERGE_REQUEST_SOURCE_BRANCH_SHA:-${CI_COMMIT_SHA:-HEAD}}"

if ! ci_wifi_lib_archives_changed "$BASE" "$HEAD"; then
    echo "No Wi-Fi library archive changes in this MR; skipping."
    exit 0
fi

description="${CI_MERGE_REQUEST_DESCRIPTION:-}"

if [[ -z "${description}" ]]; then
    echo "ERROR: merge request description is empty." >&2
    exit 1
fi

vnc_mr="$(
    printf '%s\n' "${description}" |
        sed -n -E 's/^[[:space:]]*VNC MR:[[:space:]]*(.*[^[:space:]])[[:space:]]*$/\1/p' |
        head -n 1
)"

if [[ -z "${vnc_mr}" ]]; then
    echo "ERROR: this MR updates esp/esp_driver/lib/*.a, so the description must set:" >&2
    echo "  VNC MR: 4267" >&2
    exit 1
fi

case "${vnc_mr,,}" in
    todo|tbd|n/a|na|none)
        echo "ERROR: VNC MR: is a placeholder (${vnc_mr}). Set the VNC merge-request number." >&2
        echo "Example: VNC MR: 4267" >&2
        exit 1
        ;;
esac

if ! grep -Eq '^[0-9]+$|^VNC[[:space:]]*(MR[[:space:]]*)?[0-9]+$' <<<"${vnc_mr}"; then
    echo "ERROR: VNC MR: must be a VNC merge-request number, got '${vnc_mr}'." >&2
    echo "Example: VNC MR: 4267" >&2
    exit 1
fi

echo "VNC MR from description: ${vnc_mr}"
echo "MR VNC-LIB POLICY PASS"
