#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=git-base.sh
source "${SCRIPT_DIR}/git-base.sh"

if [[ ! -f .pre-commit-config.yaml ]]; then
    echo "ERROR: .pre-commit-config.yaml is missing" >&2
    exit 1
fi

BASE="$(ci_resolve_diff_base)"
HEAD="${CI_COMMIT_SHA:-HEAD}"

echo "Running pre-commit for ${BASE}..${HEAD}"
mapfile -t files < <(git diff --name-only --diff-filter=ACMR "$BASE" "$HEAD")

if [[ "${#files[@]}" -eq 0 ]]; then
    echo "No changed files."
    exit 0
fi

printf 'Changed files:\n'
printf '  %s\n' "${files[@]}"
CI=true pre-commit run --files "${files[@]}"
