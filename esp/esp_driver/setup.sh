#!/usr/bin/env bash
set -e

ESP_IDF_DIR="esp-idf"
FORCE=0
UPDATE_IDF=0

apply_idf_patch_once() {
    local patch="$1"

    if git apply --recount --reverse --check "$patch" >/dev/null 2>&1; then
        echo "ESP hosted: patch already applied: $patch"
        return 0
    fi
    git apply --recount --check "$patch"
    git apply --recount "$patch"
}

apply_hosted_idf_patches() {
    apply_idf_patch_once ../lib/rom.patch

    # The pinned ESP-IDF v6.1 revision does not contain the generic OpenThread
    # custom RCP host transport. If a future IDF already provides it, do not
    # attempt the compatibility backport.
    if grep -q "config OPENTHREAD_RCP_CUSTOM" components/openthread/Kconfig; then
        echo "ESP hosted: native OpenThread custom RCP transport present"
        return 0
    fi

    apply_idf_patch_once ../lib/idf-openthread-custom-rcp-v6.1.patch
}

show_help() {
    echo "Usage: ./setup.sh [options]"
    echo ""
    echo "Options:"
    echo "  -f               Force reset and re-clone esp-idf (will delete all local changes)"
    echo "  -u, --update-idf Only update esp-idf to specific commit (resets esp-idf dir only)"
    echo "  -h, --help       Show this help message"
    exit 0
}

# Parse CLI arguments
for arg in "$@"; do
    case "$arg" in
        -f)
            FORCE=1
            ;;
        -u|--update-idf)
            UPDATE_IDF=1
            ;;
        -h|--help)
            show_help
            ;;
        *)
            echo "Unknown option: $arg"
            show_help
            ;;
    esac
done

# Load .env
if [ ! -f .env ]; then
    echo "ERROR: .env file not found!"
    exit 1
fi

source .env

# Check required env variables
if [ -z "$IDF_TAG" ] || [ -z "$IDF_COMMIT" ]; then
    echo "ERROR: IDF_TAG or IDF_COMMIT not defined in .env"
    exit 1
fi

# Check if esp-idf exists and its commit
ESP_IDF_PRESENT=0
ESP_IDF_CORRECT=0

if [ -d "$ESP_IDF_DIR" ]; then
    ESP_IDF_PRESENT=1
    cd "$ESP_IDF_DIR"
    CURRENT_COMMIT=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
    cd ..
    if [ "$CURRENT_COMMIT" == "$IDF_COMMIT" ]; then
        ESP_IDF_CORRECT=1
    fi
fi

# Block if commit mismatch and not -f or -u
if [ $ESP_IDF_PRESENT -eq 1 ] && [ $ESP_IDF_CORRECT -eq 0 ] && [ $FORCE -eq 0 ] && [ $UPDATE_IDF -eq 0 ]; then
    echo "ERROR: esp-idf is at $CURRENT_COMMIT but expected $IDF_COMMIT."
    echo "Use -f to reset or -u to update."
    exit 1
fi

# Handle -f
if [ $FORCE -eq 1 ]; then
    echo "WARNING: This will reset the repo and delete all local changes."
    read -p "Continue? [y/N]: " confirm
    if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
        echo "Aborted."
        exit 1
    fi

    echo "Resetting main repo"
    git reset --hard
    rm -rf "$ESP_IDF_DIR"
fi

# Clone esp-idf if not present (for all cases)
if [ ! -d "$ESP_IDF_DIR" ]; then
    echo "ESP hosted: cloning esp-idf at commit $IDF_COMMIT (tag: $IDF_TAG)"
    git clone --branch "$IDF_TAG" --depth 100 https://github.com/espressif/esp-idf.git "$ESP_IDF_DIR"
    cd "$ESP_IDF_DIR"
    git checkout -f "$IDF_COMMIT"
    echo "ESP hosted: applying IDF patches"
    apply_hosted_idf_patches
    echo "ESP hosted: initializing submodules"
    git submodule update --init --depth 1 --recursive
    echo "ESP hosted: installing prerequisites for esp-idf"
    ./install.sh
    cd ..
    ESP_IDF_PRESENT=1
    ESP_IDF_CORRECT=1
fi

# Handle -u
if [ $UPDATE_IDF -eq 1 ]; then
    echo "WARNING: This will reset changes inside esp-idf only."
    read -p "Continue? [y/N]: " confirm
    if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
        echo "Aborted."
        exit 1
    fi

    if [ ! -d "$ESP_IDF_DIR" ]; then
        echo "esp-idf not found. Cloning it now..."
        git clone --branch "$IDF_TAG" --depth 100 https://github.com/espressif/esp-idf.git "$ESP_IDF_DIR"
    fi

    echo "Updating esp-idf to commit $IDF_COMMIT"
    cd "$ESP_IDF_DIR"
    git fetch --no-recurse-submodules --depth 100 origin "$IDF_TAG"
    git reset --hard "$IDF_COMMIT"
    git clean -fdx
    echo "ESP hosted: applying IDF patches"
    apply_hosted_idf_patches
    echo "ESP hosted: updating submodules"
    git submodule update --init --depth 1 --recursive
    echo "ESP hosted: installing prerequisites for esp-idf"
    ./install.sh
    cd ..

    ESP_IDF_CORRECT=1
fi

# Existing correct checkouts may predate a newly added patch. Re-run the
# idempotent patch step on every setup invocation.
cd "$ESP_IDF_DIR"
apply_hosted_idf_patches
cd ..

echo "ESP hosted: replacing wireless libraries"
mkdir -p "$ESP_IDF_DIR/components/esp_wifi/lib"
rm -rf "$ESP_IDF_DIR/components/esp_wifi/lib/"*
cp -r lib/* "$ESP_IDF_DIR/components/esp_wifi/lib/"

echo "###### Setup Done ######"
