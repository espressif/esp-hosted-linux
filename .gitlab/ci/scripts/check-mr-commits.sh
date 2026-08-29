#!/usr/bin/env bash
set -euo pipefail

if [[ "${CI_PIPELINE_SOURCE:-}" != "merge_request_event" ]]; then
    echo "Not a merge-request pipeline; skipping."
    exit 0
fi

base="${CI_MERGE_REQUEST_DIFF_BASE_SHA:-}"
head="${CI_MERGE_REQUEST_SOURCE_BRANCH_SHA:-}"

if [[ -z "${head}" ]]; then
    head="${CI_COMMIT_SHA:-HEAD}"
fi

if [[ -z "${base}" ]]; then
    echo "ERROR: CI_MERGE_REQUEST_DIFF_BASE_SHA is unavailable." >&2
    exit 1
fi

git cat-file -e "${base}^{commit}" 2>/dev/null || {
    echo "ERROR: MR base commit ${base} is unavailable." >&2
    echo "The governance job requires GIT_DEPTH=0." >&2
    exit 1
}

git cat-file -e "${head}^{commit}" 2>/dev/null || {
    echo "ERROR: MR head commit ${head} is unavailable." >&2
    exit 1
}

mapfile -t commits < <(git rev-list --reverse "${base}..${head}")

if (( ${#commits[@]} == 0 )); then
    echo "ERROR: no commits found in MR range ${base}..${head}" >&2
    exit 1
fi

fail=0

echo "Checking ${#commits[@]} MR commit(s)..."

for sha in "${commits[@]}"; do
    short="$(git rev-parse --short=12 "${sha}")"
    subject="$(git show -s --format='%s' "${sha}")"
    author_name="$(git show -s --format='%an' "${sha}")"
    author_email="$(git show -s --format='%ae' "${sha}")"
    parents="$(git show -s --format='%P' "${sha}")"

    read -r -a parent_array <<< "${parents}"

    echo
    echo "${short} ${subject}"

    if (( ${#parent_array[@]} > 1 )); then
        echo "ERROR: merge commits are not allowed inside an MR: ${short}" >&2
        fail=1
    fi

    expected="Signed-off-by: ${author_name} <${author_email}>"
    message="$(git show -s --format='%B' "${sha}")"

    if ! printf '%s\n' "${message}" |
            git interpret-trailers --parse |
            grep -Fxiq -- "${expected}"; then
        echo "ERROR: commit is missing its author's sign-off:" >&2
        echo "       ${expected}" >&2
        fail=1
    else
        echo "Author sign-off: OK"
    fi
done

if (( fail != 0 )); then
    cat >&2 <<'EOF'

Create new commits with:

  git commit -s

To add Signed-off-by to all commits you authored on the current topic branch:

  git fetch origin master
  git rebase --signoff origin/master
  git push --force-with-lease

Do not add another person's Signed-off-by trailer yourself.
Maintainer approval belongs in GitLab's merge-request approval record.
EOF
    exit 1
fi

echo
echo "MR COMMIT POLICY PASS"
