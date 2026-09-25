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

mapfile -t decisions < <(
    printf '%s\n' "${description}" |
        sed -n -E \
            's/^[[:space:]]*Release notes decision:[[:space:]]*(.*[^[:space:]])[[:space:]]*$/\1/p'
)

if (( ${#decisions[@]} != 1 )); then
    echo "ERROR: MR description must contain exactly one Release notes decision." >&2
    echo "Use exactly one of:" >&2
    echo "  Release notes decision: added" >&2
    echo "  Release notes decision: not required" >&2
    exit 1
fi

decision="${decisions[0],,}"

case "${decision}" in
    "added"|"not required")
        ;;
    *)
        echo "ERROR: invalid Release notes decision: ${decisions[0]}" >&2
        echo "Use exactly one of:" >&2
        echo "  Release notes decision: added" >&2
        echo "  Release notes decision: not required" >&2
        exit 1
        ;;
esac

extract_block()
{
    local field="$1"
    local stop_field="${2:-}"

    printf '%s\n' "${description}" |
        awk -v field="${field}" -v stop="${stop_field}" '
            BEGIN {
                capture = 0
            }

            $0 ~ "^[[:space:]]*" field ":[[:space:]]*" {
                capture = 1
                line = $0
                sub("^[[:space:]]*" field ":[[:space:]]*", "", line)

                if (line !~ "^[[:space:]]*$")
                    print line

                next
            }

            capture {
                if (stop != "" &&
                    $0 ~ "^[[:space:]]*" stop ":[[:space:]]*")
                    exit

                if ($0 ~ "^[[:space:]]*##[[:space:]]")
                    exit

                print
            }
        ' |
        awk '
            {
                lines[NR] = $0
                if ($0 !~ /^[[:space:]]*$/) {
                    if (!first)
                        first = NR
                    last = NR
                }
            }
            END {
                if (first)
                    for (i = first; i <= last; i++)
                        print lines[i]
            }
        '
}

release_notes="$(extract_block 'Release notes' 'Skip reason')"
skip_reason="$(extract_block 'Skip reason')"

is_placeholder()
{
    local value="${1,,}"

    value="$(
        printf '%s' "${value}" |
            tr '\n' ' ' |
            sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//'
    )"

    case "${value}" in
        ""|"todo"|"tbd"|"n/a"|"na"|"none")
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

if [[ "${decision}" == "added" ]]; then
    if is_placeholder "${release_notes}"; then
        echo "ERROR: Release notes decision is 'added' but Release notes: is not completed." >&2
        exit 1
    fi

    release_notes_compact="$(printf '%s' "${release_notes}" | tr -d '[:space:]')"
    if (( ${#release_notes_compact} < 10 )); then
        echo "ERROR: release-notes text is too short." >&2
        exit 1
    fi

    echo "Release notes:"
    printf '%s\n' "${release_notes}"
else
    if is_placeholder "${skip_reason}"; then
        echo "ERROR: Release notes decision is 'not required' but Skip reason: is not completed." >&2
        exit 1
    fi

    skip_reason_compact="$(printf '%s' "${skip_reason}" | tr -d '[:space:]')"
    if (( ${#skip_reason_compact} < 5 )); then
        echo "ERROR: release-notes skip reason is too short." >&2
        exit 1
    fi

    echo "Release notes explicitly skipped:"
    printf '%s\n' "${skip_reason}"
fi

if [[ "${CI_MERGE_REQUEST_DESCRIPTION_IS_TRUNCATED:-false}" == "true" ]]; then
    echo "NOTE: GitLab truncated CI_MERGE_REQUEST_DESCRIPTION."
    echo "      Keep the Release notes section reasonably high in the MR description."
fi

echo "MR RELEASE-NOTES POLICY PASS"
