#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Espressif Systems (Shanghai) PTE LTD
"""
ESP-Hosted to ESP-HOSTED-Linux Migration & Compatibility Tool.

This tool helps developers migrating from the legacy monolithic `esp_hosted` repository
to the standalone `esp-hosted-linux` repository:
1. Identifies the current commit / branch / tag in the old repo and maps it to the equivalent commit in `esp-hosted-linux`.
2. Exports and ports uncommitted modifications and untracked files in `esp_hosted_ng/`.
3. Ports custom commits or feature branches from `esp_hosted` into `esp-hosted-linux`.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

def run_cmd(cmd, cwd=None, check=True):
    res = subprocess.run(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        shell=isinstance(cmd, str)
    )
    if check and res.returncode != 0:
        raise RuntimeError(f"Command failed ({res.returncode}): {cmd}\nStderr: {res.stderr.strip()}")
    return res

def is_git_repo(path):
    if not os.path.exists(path):
        return False
    res = subprocess.run(["git", "rev-parse", "--is-inside-work-tree"], cwd=path, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode == 0 and res.stdout.strip() == "true"

def get_repo_root(path):
    res = run_cmd(["git", "rev-parse", "--show-toplevel"], cwd=path)
    return os.path.abspath(res.stdout.strip())

def get_commit_info(repo_path, rev="HEAD"):
    res = run_cmd(["git", "log", "-n", "1", "--format=%H%n%an%n%ae%n%at%n%s%n%D", rev], cwd=repo_path, check=False)
    if res.returncode != 0:
        return None
    lines = res.stdout.strip().split("\n")
    if len(lines) < 5:
        return None
    return {
        "hash": lines[0],
        "author_name": lines[1],
        "author_email": lines[2],
        "timestamp": lines[3],
        "subject": lines[4],
        "ref_names": lines[5] if len(lines) > 5 else ""
    }

def get_all_tags(repo_path):
    res = run_cmd(["git", "show-ref", "--tags", "-d"], cwd=repo_path, check=False)
    tags = {}
    if res.returncode == 0 and res.stdout.strip():
        for line in res.stdout.strip().split("\n"):
            parts = line.strip().split()
            if len(parts) == 2:
                commit, ref = parts[0], parts[1]
                tag_name = ref.replace("refs/tags/", "").replace("^{}", "")
                tags[tag_name] = commit
    return tags

def find_matching_commit(old_repo, old_rev, new_repo):
    """Finds equivalent commit in new_repo for a given commit in old_repo."""
    old_info = get_commit_info(old_repo, old_rev)
    if not old_info:
        return None

    # 1. Search by exact subject + author name
    res = run_cmd(
        ["git", "log", "--all", f"--grep=^{re.escape(old_info['subject'])}$", f"--author={re.escape(old_info['author_name'])}", "--format=%H%n%s%n%at"],
        cwd=new_repo,
        check=False
    )
    if res.returncode == 0 and res.stdout.strip():
        entries = res.stdout.strip().split("\n")
        # Match nearest timestamp if multiple commits have identical subjects
        candidates = []
        for i in range(0, len(entries), 3):
            if i + 2 < len(entries):
                c_hash = entries[i]
                c_subj = entries[i+1]
                c_time = entries[i+2]
                candidates.append((c_hash, c_subj, c_time))
        if candidates:
            # Sort by timestamp difference
            old_time = int(old_info["timestamp"]) if old_info["timestamp"].isdigit() else 0
            candidates.sort(key=lambda c: abs(int(c[2]) - old_time) if c[2].isdigit() else 999999999)
            return get_commit_info(new_repo, candidates[0][0])

    # 2. Check if old_rev corresponds to a known tag
    old_tags = get_all_tags(old_repo)
    new_tags = get_all_tags(new_repo)
    for tag_name, tag_commit in old_tags.items():
        if tag_commit == old_info["hash"] and tag_name in new_tags:
            return get_commit_info(new_repo, new_tags[tag_name])

    return None

def check_uncommitted_changes(old_repo):
    """Checks for modified / staged / untracked files in esp_hosted_ng."""
    # Staged and unstaged tracked changes
    diff_res = run_cmd(["git", "diff", "HEAD", "--", "esp_hosted_ng"], cwd=old_repo, check=False)
    has_diff = bool(diff_res.stdout.strip())

    # Untracked files in esp_hosted_ng
    untracked_res = run_cmd(["git", "ls-files", "--others", "--exclude-standard", "esp_hosted_ng"], cwd=old_repo, check=False)
    untracked_files = [f for f in untracked_res.stdout.strip().split("\n") if f]

    return has_diff, diff_res.stdout, untracked_files

def generate_remapped_patch(raw_diff):
    """Remaps a/esp_hosted_ng/ and b/esp_hosted_ng/ to a/ and b/."""
    lines = raw_diff.split("\n")
    remapped = []
    for line in lines:
        if line.startswith("diff --git a/esp_hosted_ng/") and " b/esp_hosted_ng/" in line:
            line = line.replace(" a/esp_hosted_ng/", " a/").replace(" b/esp_hosted_ng/", " b/")
        elif line.startswith("--- a/esp_hosted_ng/"):
            line = "--- a/" + line[len("--- a/esp_hosted_ng/"):]
        elif line.startswith("+++ b/esp_hosted_ng/"):
            line = "+++ b/" + line[len("+++ b/esp_hosted_ng/"):]
        remapped.append(line)
    return "\n".join(remapped)

def command_info(old_repo, new_repo):
    print("=" * 65)
    print(" ESP-Hosted -> ESP-HOSTED-Linux Repository Migration Status")
    print("=" * 65)
    print(f"Old Repository Path: {old_repo}")
    print(f"New Repository Path: {new_repo}\n")

    old_head = get_commit_info(old_repo, "HEAD")
    new_head = get_commit_info(new_repo, "HEAD")

    print(f"[Old Repo HEAD]")
    print(f"  Commit  : {old_head['hash']}")
    print(f"  Author  : {old_head['author_name']} <{old_head['author_email']}>")
    print(f"  Subject : {old_head['subject']}")
    if old_head['ref_names']:
        print(f"  Refs    : {old_head['ref_names']}")
    print()

    print(f"[New Repo HEAD]")
    print(f"  Commit  : {new_head['hash']}")
    print(f"  Author  : {new_head['author_name']} <{new_head['author_email']}>")
    print(f"  Subject : {new_head['subject']}")
    print()

    # Find matching commit in new repo
    matching = find_matching_commit(old_repo, "HEAD", new_repo)
    print("[Commit Mapping]")
    if matching:
        print(f"  ✓ Matching base commit found in new repo:")
        print(f"    Hash    : {matching['hash']}")
        print(f"    Subject : {matching['subject']}")
    else:
        print("  ! Current HEAD in old repo has no exact equivalent commit in new repo.")
        print("    (You may be on custom local commits. Use 'port-commits' to migrate them).")
    print()

    # Check uncommitted changes
    has_diff, raw_diff, untracked = check_uncommitted_changes(old_repo)
    print("[Local Work / Uncommitted State in esp_hosted_ng]")
    if not has_diff and not untracked:
        print("  ✓ No uncommitted changes or untracked files found in esp_hosted_ng/.")
    else:
        if has_diff:
            print("  • Modified tracked files present in esp_hosted_ng/.")
        if untracked:
            print(f"  • {len(untracked)} untracked file(s) present in esp_hosted_ng/:")
            for u in untracked[:10]:
                print(f"    - {u}")
            if len(untracked) > 10:
                print(f"    ... and {len(untracked) - 10} more.")
        print("\n  Tip: Run with `apply-changes` to port these modifications to esp-hosted-linux.")
    print("=" * 65)

def command_apply_changes(old_repo, new_repo, dry_run=False):
    print(f"Porting uncommitted changes from {old_repo}/esp_hosted_ng to {new_repo}...")
    has_diff, raw_diff, untracked = check_uncommitted_changes(old_repo)

    if not has_diff and not untracked:
        print("No uncommitted changes or untracked files found in old repository.")
        return

    # 1. Apply diff
    if has_diff:
        remapped_patch = generate_remapped_patch(raw_diff)
        patch_file = os.path.join(new_repo, "uncommitted_migration.patch")
        with open(patch_file, "w", encoding="utf-8") as f:
            f.write(remapped_patch)
        print(f"Generated remapped patch: {patch_file}")

        check_res = subprocess.run(["git", "apply", "--check", patch_file], cwd=new_repo, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if check_res.returncode != 0:
            print("WARNING: Direct patch apply dry-run had conflicts:")
            print(check_res.stderr)
            print(f"Patch file saved at {patch_file}. You can apply manually with: git apply --reject {patch_file}")
        else:
            if not dry_run:
                run_cmd(["git", "apply", patch_file], cwd=new_repo)
                print("✓ Successfully applied tracked changes.")
                os.remove(patch_file)
            else:
                print("✓ Patch dry-run verified successfully.")

    # 2. Copy untracked files
    if untracked:
        print(f"Copying {len(untracked)} untracked file(s)...")
        for u in untracked:
            src_file = os.path.join(old_repo, u)
            # Remove 'esp_hosted_ng/' prefix for dest
            rel_dest = u[len("esp_hosted_ng/"):].lstrip("/")
            dst_file = os.path.join(new_repo, rel_dest)
            os.makedirs(os.path.dirname(dst_file), exist_ok=True)
            if not dry_run:
                shutil.copy2(src_file, dst_file)
                print(f"  Copied: {rel_dest}")
            else:
                print(f"  [Dry-run] Would copy: {rel_dest}")

    print("\n✓ Migration of uncommitted changes completed.")

def command_port_commits(old_repo, new_repo, base_rev, branch_name="migrated-feature"):
    print(f"Porting commits from {old_repo} (since {base_rev}) to new branch '{branch_name}' in {new_repo}...")

    # Get commits on old repo affecting esp_hosted_ng
    res = run_cmd(["git", "rev-list", "--reverse", f"{base_rev}..HEAD", "--", "esp_hosted_ng"], cwd=old_repo)
    commits = [c.strip() for c in res.stdout.strip().split("\n") if c.strip()]

    if not commits:
        print(f"No commits found affecting esp_hosted_ng between {base_rev} and HEAD.")
        return

    print(f"Found {len(commits)} commit(s) to port.")

    # Create new branch in new repo
    run_cmd(["git", "checkout", "-b", branch_name], cwd=new_repo)

    patch_dir = os.path.join(new_repo, ".migration_patches")
    os.makedirs(patch_dir, exist_ok=True)

    for i, commit in enumerate(commits):
        c_info = get_commit_info(old_repo, commit)
        raw_patch = run_cmd(["git", "format-patch", "-1", commit, "--stdout", "--", "esp_hosted_ng"], cwd=old_repo).stdout
        remapped_patch = generate_remapped_patch(raw_patch)

        p_path = os.path.join(patch_dir, f"{i:04d}.patch")
        with open(p_path, "w", encoding="utf-8") as f:
            f.write(remapped_patch)

        apply_res = subprocess.run(["git", "am", p_path], cwd=new_repo, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if apply_res.returncode == 0:
            print(f"  ✓ [{i+1}/{len(commits)}] Ported commit: {c_info['subject']}")
        else:
            print(f"  ! Conflict applying commit: {c_info['subject']}")
            print("    Please resolve conflicts and run `git am --continue` or abort with `git am --abort`.")
            print(f"    Patch file: {p_path}")
            return

    shutil.rmtree(patch_dir, ignore_errors=True)
    print(f"\n✓ Successfully ported all commits to branch '{branch_name}' in {new_repo}.")

def main():
    default_new_repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

    parser = argparse.ArgumentParser(description="Migration utility from legacy esp_hosted to esp-hosted-linux.")
    parser.add_argument("command", nargs="?", default="info", choices=["info", "apply-changes", "port-commits"], help="Action to perform (default: info)")
    parser.add_argument("--old-repo", help="Path to old esp_hosted repository")
    parser.add_argument("--new-repo", default=default_new_repo, help="Path to new esp-hosted-linux repository (default: current repository)")
    parser.add_argument("--base-rev", default="origin/master", help="Base revision for port-commits (default: origin/master)")
    parser.add_argument("--branch", default="migrated-feature", help="Target branch name for ported commits (default: migrated-feature)")
    parser.add_argument("--dry-run", action="store_true", help="Simulate applying changes without modifying files")

    args = parser.parse_args()

    # Determine old repo path if not specified
    old_repo = args.old_repo
    if not old_repo:
        # Check standard adjacent paths
        candidates = [
            os.path.abspath(os.path.join(args.new_repo, "..", "esp_hosted")),
            os.path.abspath(os.path.join(args.new_repo, "..", "..", "esp_hosted")),
            "/Users/kapilgupta/test/esp_hosted"
        ]
        for c in candidates:
            if is_git_repo(c):
                old_repo = c
                break

    if not old_repo or not is_git_repo(old_repo):
        print(f"Error: Could not locate valid old `esp_hosted` repository. Please specify with `--old-repo <path>`.")
        sys.exit(1)

    if not is_git_repo(args.new_repo):
        print(f"Error: '{args.new_repo}' is not a valid git repository.")
        sys.exit(1)

    old_repo = get_repo_root(old_repo)
    new_repo = get_repo_root(args.new_repo)

    if args.command == "info":
        command_info(old_repo, new_repo)
    elif args.command == "apply-changes":
        command_apply_changes(old_repo, new_repo, dry_run=args.dry_run)
    elif args.command == "port-commits":
        command_port_commits(old_repo, new_repo, base_rev=args.base_rev, branch_name=args.branch)

if __name__ == "__main__":
    main()
