#!/usr/bin/env python3
"""Check or refresh the fixed source-file transfer manifest in this directory."""

import argparse
import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "SOURCE_MANIFEST.sha256"


def listed_files() -> list[str]:
    names = []
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        digest, separator, name = line.partition("  ")
        if len(digest) != 64 or not separator or not name:
            raise ValueError("invalid manifest entry")
        path = (ROOT / name).resolve()
        if ROOT not in path.parents or not path.is_file():
            raise ValueError(f"missing or unsafe manifest file: {name}")
        names.append(name)
    if len(set(names)) != len(names):
        raise ValueError("duplicate manifest entry")
    if "scripts/manifest.py" not in names:
        names.append("scripts/manifest.py")
    return sorted(names)


def render(names: list[str]) -> str:
    return "".join(
        f"{hashlib.sha256((ROOT / name).read_bytes()).hexdigest()}  {name}\n"
        for name in names
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true")
    group.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    expected = render(listed_files())
    if args.check:
        if MANIFEST.read_text(encoding="utf-8") != expected:
            parser.error("manifest is stale; inspect changes before --refresh")
        print("SOURCE_MANIFEST_OK")
    else:
        MANIFEST.write_text(expected, encoding="utf-8")
        print("SOURCE_MANIFEST_REFRESHED")


if __name__ == "__main__":
    main()
