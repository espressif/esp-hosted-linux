# Migration Guide: Upgrading from `esp_hosted` to `esp-hosted-linux`

This guide explains how to migrate existing workflows, custom branches, and local modifications from the legacy monolithic `esp_hosted` repository (`esp_hosted_ng/` directory) to the new dedicated `esp-hosted-linux` repository.

---

## 1. What Changed?

- **Dedicated Repository:** Next-Generation Linux support (`esp_hosted_ng`) is now maintained in its own dedicated repository named **`esp-hosted-linux`**.
- **Root Directory Layout:** Files previously under `esp_hosted_ng/` (e.g. `esp_hosted_ng/esp/` and `esp_hosted_ng/host/`) are now directly at the root of `esp-hosted-linux/` (`esp/` and `host/`).
- **Preserved History & Tags:** All 260+ commits and official release tags (`release/ng-1.0.6`, `release/ng-1.0.5.0.3`, `release/ng-1.0.4.0.0`, `release/ng-v1.0.4.0.0`, `release/ng-v1.0.2`) have been fully preserved.

---

## 2. Quick Migration Tool

A helper migration script is provided in `tools/migrate_from_esp_hosted.py`.

### Step 1: Clone the new `esp-hosted-linux` repository
```bash
git clone <NEW_ESP_HOSTED_LINUX_REPO_URL> esp-hosted-linux
cd esp-hosted-linux
```

### Step 2: Check your old repository state
To see which commit/tag in `esp-hosted-linux` corresponds to your current checkout in the old repository:
```bash
python3 tools/migrate_from_esp_hosted.py info --old-repo /path/to/old/esp_hosted
```

### Step 3: Migrate uncommitted work / untracked files
If you have work-in-progress modifications in `esp_hosted/esp_hosted_ng/`:
```bash
# Dry-run inspection
python3 tools/migrate_from_esp_hosted.py apply-changes --old-repo /path/to/old/esp_hosted --dry-run

# Apply changes to esp-hosted-linux
python3 tools/migrate_from_esp_hosted.py apply-changes --old-repo /path/to/old/esp_hosted
```

### Step 4: Port custom feature commits / branches
If you have committed changes on a custom branch in your old repository:
```bash
python3 tools/migrate_from_esp_hosted.py port-commits \
  --old-repo /path/to/old/esp_hosted \
  --base-rev origin/master \
  --branch my-migrated-feature
```

---

## 3. Directory & Path Mapping Reference

| Legacy Path in `esp_hosted` | New Path in `esp-hosted-linux` |
|---|---|
| `esp_hosted_ng/esp/esp_driver/` | `esp/esp_driver/` |
| `esp_hosted_ng/esp/esp_driver/network_adapter/` | `esp/esp_driver/network_adapter/` |
| `esp_hosted_ng/host/` | `host/` |
| `esp_hosted_ng/docs/` | `docs/` |
| `esp_hosted_ng/README.md` | `README.md` |
| `esp_hosted_ng/VERSION` | `VERSION` |

---

## 4. Manual Patch Export (Alternative)

If you prefer using standard git commands:

### To export uncommitted changes:
```bash
# In the old esp_hosted repo:
git diff HEAD -- esp_hosted_ng | sed -e 's| a/esp_hosted_ng/| a/|g' -e 's| b/esp_hosted_ng/| b/|g' -e 's|--- a/esp_hosted_ng/|--- a/|g' -e 's|+++ b/esp_hosted_ng/|+++ b/|g' > patch.diff

# In esp-hosted-linux repo:
git apply patch.diff
```

### To format and port a commit:
```bash
# In the old esp_hosted repo:
git format-patch -1 <COMMIT_HASH> --stdout -- esp_hosted_ng | sed -e 's| a/esp_hosted_ng/| a/|g' -e 's| b/esp_hosted_ng/| b/|g' -e 's|--- a/esp_hosted_ng/|--- a/|g' -e 's|+++ b/esp_hosted_ng/|+++ b/|g' > 0001.patch

# In esp-hosted-linux repo:
git am 0001.patch
```
