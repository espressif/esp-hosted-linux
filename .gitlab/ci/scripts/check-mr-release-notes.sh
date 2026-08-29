#!/usr/bin/env bash
set -euo pipefail

if [[ "${CI_PIPELINE_SOURCE:-}" != "merge_request_event" ]]; then
    echo "Not a merge-request pipeline; skipping."
    exit 0
fi

description="${CI_MERGE_REQUEST_DESCRIPTION:-}"

if [[ -z "${description}" ]]; then
    echo "ERROR: merge request description is empty." >&2
    exit 1
fi

checked_added="$(
    printf '%s\n' "${description}" |
        grep -Eic '^[[:space:]]*-[[:space:]]*\[[xX]\][[:space:]]*Release note added[[:space:]]*$' || true
)"

checked_skip="$(
    printf '%s\n' "${description}" |
        grep -Eic '^[[:space:]]*-[[:space:]]*\[[xX]\][[:space:]]*Release note not required[[:space:]]*$' || true
)"

selected=$((checked_added + checked_skip))

if (( selected != 1 )); then
    echo "ERROR: select exactly one release-note option in the MR description:" >&2
    echo "  - [x] Release note added" >&2
    echo "  - [x] Release note not required" >&2
    exit 1
fi

extract_field()
{
    local field="$1"
    printf '%s\n' "${description}" |
        sed -n -E "s/^[[:space:]]*${field}:[[:space:]]*(.*[^[:space:]])[[:space:]]*$/\1/p" |
        head -n 1
}

release_note="$(extract_field 'Release note')"
skip_reason="$(extract_field 'Skip reason')"

is_placeholder()
{
    local value="${1,,}"
    case "${value}" in
        ""|"todo"|"tbd"|"n/a"|"na"|"none") return 0 ;;
        *) return 1 ;;
    esac
}

if (( checked_added == 1 )); then
    if is_placeholder "${release_note}"; then
        echo "ERROR: 'Release note added' is selected but Release note: is not completed." >&2
        exit 1
    fi
    if (( ${#release_note} < 10 )); then
        echo "ERROR: release-note text is too short." >&2
        exit 1
    fi
    echo "Release note:"
    echo "  ${release_note}"
fi

if (( checked_skip == 1 )); then
    if is_placeholder "${skip_reason}"; then
        echo "ERROR: 'Release note not required' is selected but Skip reason: is not completed." >&2
        exit 1
    fi
    if (( ${#skip_reason} < 5 )); then
        echo "ERROR: release-note skip reason is too short." >&2
        exit 1
    fi
    echo "Release note explicitly skipped:"
    echo "  ${skip_reason}"
fi

if [[ "${CI_MERGE_REQUEST_DESCRIPTION_IS_TRUNCATED:-false}" == "true" ]]; then
    echo "NOTE: GitLab truncated CI_MERGE_REQUEST_DESCRIPTION."
    echo "      Keep the Release notes section reasonably high in the MR description."
fi

echo "MR RELEASE-NOTES POLICY PASS"
