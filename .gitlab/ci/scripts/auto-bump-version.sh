#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=git-base.sh
source "${SCRIPT_DIR}/git-base.sh"

FW_ROOT="esp/esp_driver/network_adapter"
FW_VERSION="${FW_ROOT}/main/include/esp_fw_version.h"
HOST_VERSION="host/include/esp_fw_version.h"
VERSION_TOOL="${SCRIPT_DIR}/version_header.py"

[[ "${CI_PIPELINE_SOURCE:-}" == "merge_request_event" ]] || {
    echo "Not an MR pipeline; automatic version bump not required."
    exit 0
}

[[ "${CI_MERGE_REQUEST_SOURCE_PROJECT_ID:-}" == "${CI_PROJECT_ID:-}" ]] || {
    echo "MR comes from another project/fork; automatic push is disabled."
    exit 0
}

BASE="$(ci_resolve_diff_base)"
HEAD="${CI_COMMIT_SHA:-HEAD}"

echo "Firmware version policy base: ${BASE}"
mapfile -t changed < <(git diff --name-only "$BASE" "$HEAD")

firmware_changed=0
version_touched=0

for path in "${changed[@]}"; do
    case "$path" in
        "$FW_VERSION"|"$HOST_VERSION")
            version_touched=1
            ;;
    esac

    case "$path" in
        "$FW_ROOT"/*)
            # Version header itself is the consequence, not a reason to bump.
            [[ "$path" == "$FW_VERSION" ]] && continue
            # Documentation-only changes do not change the firmware image/API.
            case "$path" in
                *.md|*.rst|"$FW_ROOT"/docs/*|"$FW_ROOT"/README*) continue ;;
            esac
            firmware_changed=1
            ;;
    esac
done

if [[ "$firmware_changed" -eq 0 ]]; then
    echo "No firmware-bearing network_adapter changes; no version bump required."
    exit 0
fi

if [[ "$version_touched" -eq 1 ]]; then
    echo "Developer already touched a version header; validating manual bump."

    tmp="$(mktemp)"
    trap 'rm -f "$tmp"' EXIT
    git show "${BASE}:${FW_VERSION}" > "$tmp"
    python3 "$VERSION_TOOL" require-greater "$tmp" "$FW_VERSION"

    if ! cmp -s "$FW_VERSION" "$HOST_VERSION"; then
        echo "ERROR: manual version bump must update both mirrored version headers." >&2
        diff -u "$HOST_VERSION" "$FW_VERSION" || true
        exit 1
    fi

    exit 0
fi

if [[ "${AUTO_BUMP_DRY_RUN:-0}" != "1" ]]; then
    [[ -n "${VERSION_BOT_TOKEN:-}" ]] || {
        cat >&2 <<'EOF'
ERROR: firmware changed and no manual version bump was supplied, but VERSION_BOT_TOKEN is not set.
Create a project access token with Developer (or sufficient push role) and write_repository scope,
then store it as the masked CI/CD variable VERSION_BOT_TOKEN.
EOF
        exit 1
    }

    source_branch="${CI_MERGE_REQUEST_SOURCE_BRANCH_NAME:?missing source branch}"

    # Do not commit on top of a stale pipeline if the developer already pushed again.
    git fetch origin "$source_branch" --depth=1 >/dev/null
    remote_sha="$(git rev-parse FETCH_HEAD)"
    if [[ "$remote_sha" != "$HEAD" ]]; then
        echo "Source branch advanced to ${remote_sha}; skipping bump from stale pipeline ${HEAD}."
        exit 0
    fi
fi

old_version="$(python3 "$VERSION_TOOL" print "$FW_VERSION")"
change="$(python3 "$VERSION_TOOL" bump-patch2 "$FW_VERSION")"
cp "$FW_VERSION" "$HOST_VERSION"
new_version="$(python3 "$VERSION_TOOL" print "$FW_VERSION")"

echo "Automatic firmware version bump: ${change}"

if [[ "${AUTO_BUMP_DRY_RUN:-0}" == "1" ]]; then
    echo "AUTO_BUMP_DRY_RUN=1: not committing or pushing."
    git diff -- "$FW_VERSION" "$HOST_VERSION"
    exit 0
fi

git config user.name "ESP-Hosted Linux Version Bot"
git config user.email "esp-hosted-linux-version-bot@espressif.com"
git add "$FW_VERSION" "$HOST_VERSION"
git commit -m "ci: bump version ${old_version} -> ${new_version}"

push_url="${CI_SERVER_URL/\/\//\/\/oauth2:${VERSION_BOT_TOKEN}@}/${CI_PROJECT_PATH}.git"
# Never echo push_url: it contains the token.
git push "$push_url" "HEAD:${source_branch}"

echo "Pushed automatic version bump to ${source_branch}."
echo "A newer pipeline will validate/build the bumped MR head."
