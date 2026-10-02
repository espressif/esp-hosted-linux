# Migrating from `esp_hosted` to `esp-hosted-linux`

The Linux implementation that lived under `esp_hosted_ng/` was split into this standalone repository. Git history was retained, while project files moved to repository root.

`ORIGIN.md` records the split point and upstream repository.

## Path mapping

| Old path | New path |
|---|---|
| `esp_hosted_ng/esp/` | `esp/` |
| `esp_hosted_ng/esp/esp_driver/network_adapter/` | `esp/esp_driver/network_adapter/` |
| `esp_hosted_ng/host/` | `host/` |
| `esp_hosted_ng/docs/` | `docs/` |
| `esp_hosted_ng/README.md` | `README.md` |
| `esp_hosted_ng/VERSION` | `VERSION` |

## Migration helper

`tools/migrate_from_esp_hosted.py` can inspect an old checkout, move uncommitted work, or port commits.

Start from a clean `esp-hosted-linux` checkout and check its state before applying changes:

```sh
git status --short
```

Inspect an old repository:

```sh
python3 tools/migrate_from_esp_hosted.py info \
  --old-repo /path/to/old/esp_hosted
```

Preview uncommitted changes:

```sh
python3 tools/migrate_from_esp_hosted.py apply-changes \
  --old-repo /path/to/old/esp_hosted \
  --dry-run
```

Apply them:

```sh
python3 tools/migrate_from_esp_hosted.py apply-changes \
  --old-repo /path/to/old/esp_hosted
```

Port commits to a new branch:

```sh
python3 tools/migrate_from_esp_hosted.py port-commits \
  --old-repo /path/to/old/esp_hosted \
  --base-rev origin/master \
  --branch my-migrated-feature
```

Review `git status` and the resulting diff before committing or pushing migrated work.

## Manual Git workflow

For committed work, `git format-patch` and `git am` can be simpler when you want full control over history.

When adjusting patches by hand, remove the old `esp_hosted_ng/` path prefix before applying them to this repository.
