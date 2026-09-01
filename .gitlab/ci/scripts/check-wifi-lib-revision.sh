#!/usr/bin/env bash
# When an MR updates prebuilt Wi-Fi archives, the latest commit that touches
# those *.a files must name the VNC remote SHA in the subject:
#
#   <summary> (2a25d4d)
#
# The VNC MR number belongs in the GitLab MR description, not the commit.
# HEAD's libraries must contain that parenthetical SHA (and SHA-remote),
# and must not contain SHA-dirty. Version-bot / non-lib commits are ignored.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=git-base.sh
source "${SCRIPT_DIR}/git-base.sh"

LIB_ROOT="esp/esp_driver/lib"
REQUIRED_ARCHIVES=(
    libcore.a
    libespnow.a
    libnet80211.a
    libpp.a
    libsmartconfig.a
)
OPTIONAL_ARCHIVES=(
    libmesh.a
    libwapi.a
    libtarget.a
)

if [[ "${CI_PIPELINE_SOURCE:-}" != "merge_request_event" ]]; then
    echo "Not a merge-request pipeline; skipping."
    exit 0
fi

BASE="$(ci_resolve_diff_base)"
HEAD="${CI_MERGE_REQUEST_SOURCE_BRANCH_SHA:-${CI_COMMIT_SHA:-HEAD}}"

git cat-file -e "${BASE}^{commit}" 2>/dev/null || {
    echo "ERROR: MR base commit ${BASE} is unavailable." >&2
    exit 1
}
git cat-file -e "${HEAD}^{commit}" 2>/dev/null || {
    echo "ERROR: MR head commit ${HEAD} is unavailable." >&2
    exit 1
}

commit_touches_wifi_libs() {
    local sha="$1"
    git diff-tree --no-commit-id --name-only -r --diff-filter=AMRD "$sha" |
        grep -Eq "^${LIB_ROOT}/.+\.a$"
}

latest_lib_commit=""
mapfile -t commits < <(git rev-list "${BASE}..${HEAD}")
for sha in "${commits[@]}"; do
    if commit_touches_wifi_libs "$sha"; then
        latest_lib_commit="$sha"
        break
    fi
done

if [[ -z "$latest_lib_commit" ]]; then
    echo "No Wi-Fi library archive changes in this MR; skipping."
    exit 0
fi

short="$(git rev-parse --short=12 "${latest_lib_commit}")"
subject="$(git show -s --format='%s' "${latest_lib_commit}")"

echo "Latest lib-updating commit: ${short}"
echo "Subject: ${subject}"

paren_sha="$(
    printf '%s\n' "$subject" |
        grep -oE '\([0-9a-f]+\)' |
        tail -n 1 |
        tr -d '()' || true
)"

if [[ -z "$paren_sha" ]]; then
    cat >&2 <<EOF
ERROR: commit ${short} updates ${LIB_ROOT}/*.a but its subject has no VNC SHA in parentheses.
Expected subject form:

  <summary> (2a25d4d)

Put the VNC MR number in the GitLab MR description, not in the commit subject.
Version-bot and other non-lib commits are not required to include the SHA.
EOF
    exit 1
fi

vnc_sha="${paren_sha:0:7}"
if [[ ${#vnc_sha} -ne 7 ]]; then
    echo "ERROR: VNC SHA in parentheses must be at least 7 hex characters, got '${paren_sha}'." >&2
    exit 1
fi

echo "VNC SHA from parentheses: ${vnc_sha}"
echo "Checking HEAD libraries against that SHA..."

fail=0

chip_dirs=()
while IFS= read -r dir; do
    chip_dirs+=("$dir")
done < <(find "$LIB_ROOT" -mindepth 1 -maxdepth 1 -type d -name 'esp32*' | sort)

if (( ${#chip_dirs[@]} == 0 )); then
    echo "ERROR: no chip library directories found under ${LIB_ROOT}" >&2
    exit 1
fi

for dir in "${chip_dirs[@]}"; do
    shopt -s nullglob
    archives=("${dir}"/*.a)
    shopt -u nullglob
    if (( ${#archives[@]} == 0 )); then
        continue
    fi

    echo
    echo "Checking ${dir}"

    for name in "${REQUIRED_ARCHIVES[@]}"; do
        archive="${dir}/${name}"
        if [[ ! -f "$archive" ]]; then
            echo "ERROR: missing required archive ${archive}" >&2
            fail=1
            continue
        fi
        if ! grep -aqF -- "$vnc_sha" "$archive"; then
            echo "ERROR: ${archive} does not contain VNC SHA ${vnc_sha} from the commit subject." >&2
            fail=1
        else
            echo "  ${name}: SHA ${vnc_sha} OK"
        fi
    done

    for name in "${OPTIONAL_ARCHIVES[@]}"; do
        archive="${dir}/${name}"
        [[ -f "$archive" ]] || continue
        if ! grep -aqF -- "$vnc_sha" "$archive"; then
            echo "ERROR: ${archive} does not contain VNC SHA ${vnc_sha} from the commit subject." >&2
            fail=1
        else
            echo "  ${name}: SHA ${vnc_sha} OK"
        fi
    done

    net80211="${dir}/libnet80211.a"
    if [[ -f "$net80211" ]]; then
        if ! grep -aqF -- "${vnc_sha}-remote" "$net80211"; then
            echo "ERROR: ${net80211} does not contain ${vnc_sha}-remote." >&2
            fail=1
        else
            echo "  libnet80211.a: ${vnc_sha}-remote OK"
        fi
    fi

    for archive in "${archives[@]}"; do
        if grep -aqF -- "${vnc_sha}-dirty" "$archive"; then
            echo "ERROR: $(basename "$archive") in ${dir} contains ${vnc_sha}-dirty." >&2
            fail=1
        fi
    done
done

if (( fail != 0 )); then
    echo >&2
    echo "ERROR: Wi-Fi libraries at HEAD do not match VNC SHA (${vnc_sha}) from commit ${short}." >&2
    exit 1
fi

echo
echo "WIFI LIB REVISION POLICY PASS"
