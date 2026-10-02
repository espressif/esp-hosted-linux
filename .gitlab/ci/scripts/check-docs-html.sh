#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="docs-html"

# Keep generated output deterministic. MkDocs includes the build time in the
# homepage HTML, so this must match the generator before byte-for-byte compare.
export SOURCE_DATE_EPOCH=0

# When the prepare-stage generator has already pushed a newer commit to the
# same source branch, this pipeline is stale. The newer pipeline will validate
# the generated HTML against that newer branch head.
source_branch=""
source_head="${CI_COMMIT_SHA:-}"

if [[ "${CI_PIPELINE_SOURCE:-}" == "merge_request_event" &&
      "${CI_MERGE_REQUEST_SOURCE_PROJECT_ID:-}" == "${CI_PROJECT_ID:-}" ]]; then
    source_branch="${CI_MERGE_REQUEST_SOURCE_BRANCH_NAME:-}"
    source_head="${CI_MERGE_REQUEST_SOURCE_BRANCH_SHA:-${CI_COMMIT_SHA:-}}"
elif [[ -n "${CI_COMMIT_BRANCH:-}" ]]; then
    source_branch="${CI_COMMIT_BRANCH}"
fi

if [[ -n "$source_branch" && -n "$source_head" ]]; then
    git fetch origin "$source_branch" --depth=1 >/dev/null
    remote_sha="$(git rev-parse FETCH_HEAD)"
    if [[ "$remote_sha" != "$source_head" ]]; then
        echo "Source branch advanced to ${remote_sha}; a newer pipeline will validate generated HTML."
        exit 0
    fi
fi

if [[ ! -d "$OUTPUT_DIR" ]]; then
    cat >&2 <<'EOF'
ERROR: docs-html/ is missing.
Documentation HTML must be generated and committed before this revision can pass validation.
EOF
    exit 1
fi

tmp_dir="$(mktemp -d /tmp/esp-hosted-docs-html.XXXXXX)"
diff_file="$(mktemp /tmp/esp-hosted-docs-html-diff.XXXXXX)"
trap 'rm -rf "$tmp_dir" "$diff_file"' EXIT

mkdocs build --strict --site-dir "$tmp_dir"

if diff -qr "$OUTPUT_DIR" "$tmp_dir" >"$diff_file"; then
    echo "Committed docs-html/ matches a fresh MkDocs build."
    exit 0
fi

cat >&2 <<'EOF'
ERROR: committed docs-html/ does not match a fresh MkDocs build.
Documentation source, MkDocs configuration, and generated HTML must stay in sync.
Do not edit docs-html/ by hand; regenerate it from the documentation source.

Differences:
EOF
sed -n '1,200p' "$diff_file" >&2
exit 1
