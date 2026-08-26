#!/usr/bin/env bash
# Helpers for MR-aware scripts running in shallow GitLab clones.

ci_resolve_diff_base() {
    local base="${CI_MERGE_REQUEST_DIFF_BASE_SHA:-}"
    local zero="0000000000000000000000000000000000000000"

    if [[ -n "$base" && "$base" != "$zero" ]]; then
        if ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
            git fetch origin "$base" --depth=1 >/dev/null 2>&1 || true
        fi
        if git cat-file -e "${base}^{commit}" 2>/dev/null; then
            printf '%s\n' "$base"
            return 0
        fi
    fi

    local target="${CI_MERGE_REQUEST_TARGET_BRANCH_NAME:-${CI_DEFAULT_BRANCH:-master}}"
    git fetch origin "$target" --depth=200 >/dev/null

    local resolved
    if resolved=$(git merge-base HEAD FETCH_HEAD 2>/dev/null); then
        printf '%s\n' "$resolved"
        return 0
    fi

    # Last resort for unusually deep/shallow histories.
    git fetch origin "$target" --unshallow >/dev/null 2>&1 || \
        git fetch origin "$target" --deepen=1000 >/dev/null 2>&1 || true
    resolved=$(git merge-base HEAD "origin/${target}" 2>/dev/null) || {
        echo "ERROR: unable to resolve MR diff base against ${target}" >&2
        return 1
    }
    printf '%s\n' "$resolved"
}
