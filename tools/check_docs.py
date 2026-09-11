#!/usr/bin/env python3
"""Validate local Markdown links and anchors in ESP-Hosted-Linux docs."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*$", re.MULTILINE)
SKIP_PREFIXES = ("http://", "https://", "mailto:", "data:")


def markdown_files() -> list[Path]:
    files = [ROOT / "README.md", ROOT / "CONTRIBUTING.md", ROOT / "ORIGIN.md"]
    files.extend(sorted((ROOT / "docs").rglob("*.md")))
    files.extend(sorted((ROOT / "LICENSES").rglob("*.md")))
    return [path for path in files if path.is_file()]


def split_destination(raw: str) -> tuple[str, str]:
    value = raw.strip()
    if value.startswith("<") and value.endswith(">"):
        value = value[1:-1]

    # Markdown permits a title after a whitespace-separated URL/path.
    if " " in value and not value.startswith(("http://", "https://")):
        value = value.split()[0]

    value = unquote(value)
    if "#" in value:
        path, anchor = value.split("#", 1)
        return path, anchor
    return value, ""


def github_anchor(text: str) -> str:
    # Good enough for headings used by this repository and by MkDocs/GitHub.
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"[`*_~]", "", text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text, flags=re.UNICODE)
    text = re.sub(r"\s+", "-", text)
    return text


def anchors_for(path: Path) -> set[str]:
    anchors: set[str] = set()
    counts: dict[str, int] = {}
    text = path.read_text(encoding="utf-8")

    for match in HEADING_RE.finditer(text):
        base = github_anchor(match.group(2))
        if not base:
            continue
        count = counts.get(base, 0)
        counts[base] = count + 1
        anchors.add(base if count == 0 else f"{base}-{count}")

    return anchors


def main() -> int:
    failures: list[str] = []
    checked_files = 0
    checked_anchors = 0
    anchor_cache: dict[Path, set[str]] = {}

    for source in markdown_files():
        text = source.read_text(encoding="utf-8")

        for match in LINK_RE.finditer(text):
            raw = match.group(1).strip()
            if not raw or raw.startswith(SKIP_PREFIXES):
                continue

            destination, anchor = split_destination(raw)
            target = source if not destination else (source.parent / destination).resolve()

            try:
                target.relative_to(ROOT)
            except ValueError:
                failures.append(
                    f"{source.relative_to(ROOT)}: link escapes repository: {raw}"
                )
                continue

            if destination:
                checked_files += 1
                if not target.exists():
                    failures.append(
                        f"{source.relative_to(ROOT)}: missing target: {raw}"
                    )
                    continue

            if anchor and target.suffix.lower() == ".md":
                checked_anchors += 1
                if target not in anchor_cache:
                    anchor_cache[target] = anchors_for(target)
                if anchor not in anchor_cache[target]:
                    failures.append(
                        f"{source.relative_to(ROOT)}: missing anchor '#{anchor}' in "
                        f"{target.relative_to(ROOT)}"
                    )

    if failures:
        print("Documentation validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "Documentation validation passed "
        f"({checked_files} local file links, {checked_anchors} anchors checked)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
