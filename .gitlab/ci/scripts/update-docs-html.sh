#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="docs-html"

# MkDocs includes its build time in generated output. Pin it so the tracked
# HTML is reproducible and can be compared byte-for-byte in a later pipeline.
export SOURCE_DATE_EPOCH=0

if [[ "${CI_PIPELINE_SOURCE:-}" == "merge_request_event" ]]; then
    [[ "${CI_MERGE_REQUEST_SOURCE_PROJECT_ID:-}" == "${CI_PROJECT_ID:-}" ]] || {
        echo "MR comes from another project/fork; automatic push is disabled."
        exit 0
    }
    source_branch="${CI_MERGE_REQUEST_SOURCE_BRANCH_NAME:?missing source branch}"
elif [[ -n "${CI_COMMIT_BRANCH:-}" ]]; then
    source_branch="${CI_COMMIT_BRANCH}"
else
    echo "Pipeline is not associated with a writable branch; automatic HTML update is not required."
    exit 0
fi

source_head="${CI_COMMIT_SHA:-HEAD}"

# Do not generate a commit on top of a stale pipeline if another automation or
# developer push has already advanced the source branch.
git fetch origin "$source_branch" --depth=1 >/dev/null
remote_sha="$(git rev-parse FETCH_HEAD)"
if [[ "$remote_sha" != "$source_head" ]]; then
    echo "Source branch advanced to ${remote_sha}; skipping HTML update from stale pipeline ${source_head}."
    exit 0
fi

rm -rf "$OUTPUT_DIR"
mkdocs build --strict
python3 tools/check_docs.py

git add -A "$OUTPUT_DIR"
if git diff --cached --quiet -- "$OUTPUT_DIR"; then
    echo "Generated HTML is already up to date."
    exit 0
fi

token="${DOCS_BOT_TOKEN:-${VERSION_BOT_TOKEN:-}}"
[[ -n "$token" ]] || {
    cat >&2 <<'EOF'
ERROR: generated documentation changed but no push token is available.
Set DOCS_BOT_TOKEN to a project access token with permission to push to the
source branch. VERSION_BOT_TOKEN is accepted as a fallback when it already has
suitable write_repository access.
EOF
    exit 1
}

git config user.name "ESP-Hosted Linux Docs Bot"
git config user.email "esp-hosted-linux-docs-bot@espressif.com"
git commit -m "docs: regenerate HTML"

push_url="${CI_SERVER_URL/\/\//\/\/oauth2:${token}@}/${CI_PROJECT_PATH}.git"
# Never echo push_url: it contains the token.
if git push "$push_url" "HEAD:${source_branch}"; then
    echo "Pushed regenerated HTML to ${source_branch}."
    exit 0
fi

# A concurrent automation may have won the race. Treat that as stale work and
# let the newer pipeline regenerate from the new branch head.
git fetch origin "$source_branch" --depth=1 >/dev/null
new_remote_sha="$(git rev-parse FETCH_HEAD)"
if [[ "$new_remote_sha" != "$source_head" ]]; then
    echo "Source branch advanced to ${new_remote_sha}; a newer pipeline will regenerate HTML."
    exit 0
fi

echo "ERROR: failed to push generated HTML while source branch remained at ${source_head}." >&2
exit 1
