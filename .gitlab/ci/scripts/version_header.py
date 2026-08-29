#!/usr/bin/env python3
import argparse
import re
from pathlib import Path

FIELDS = [
    "PROJECT_VERSION_MAJOR_1",
    "PROJECT_VERSION_MAJOR_2",
    "PROJECT_VERSION_MINOR",
    "PROJECT_REVISION_PATCH_1",
    "PROJECT_REVISION_PATCH_2",
]


def parse(path: Path):
    text = path.read_text()
    values = []
    for key in FIELDS:
        m = re.search(rf"^\s*#define\s+{re.escape(key)}\s+(\d+)\s*$", text, re.M)
        if not m:
            raise SystemExit(f"ERROR: {path}: missing numeric #define {key}")
        values.append(int(m.group(1)))
    return tuple(values)


def set_patch2(path: Path, value: int):
    text = path.read_text()
    key = "PROJECT_REVISION_PATCH_2"
    pattern = rf"^(\s*#define\s+{re.escape(key)}\s+)\d+(\s*)$"
    new, count = re.subn(pattern, rf"\g<1>{value}\g<2>", text, count=1, flags=re.M)
    if count != 1:
        raise SystemExit(f"ERROR: {path}: could not update {key}")
    path.write_text(new)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("print")
    p.add_argument("file", type=Path)

    p = sub.add_parser("bump-patch2")
    p.add_argument("file", type=Path)

    p = sub.add_parser("require-greater")
    p.add_argument("base", type=Path)
    p.add_argument("current", type=Path)

    args = ap.parse_args()

    if args.cmd == "print":
        v = parse(args.file)
        print(".".join(map(str, v)))
    elif args.cmd == "bump-patch2":
        old = parse(args.file)
        set_patch2(args.file, old[-1] + 1)
        new = parse(args.file)
        print(f"{'.'.join(map(str, old))} -> {'.'.join(map(str, new))}")
    elif args.cmd == "require-greater":
        base = parse(args.base)
        current = parse(args.current)
        if current <= base:
            raise SystemExit(
                "ERROR: firmware version was touched but was not increased: "
                f"base={'.'.join(map(str, base))}, current={'.'.join(map(str, current))}"
            )
        print(f"Version bump validated: {'.'.join(map(str, base))} -> {'.'.join(map(str, current))}")


if __name__ == "__main__":
    main()
