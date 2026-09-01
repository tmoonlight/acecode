#!/usr/bin/env python3
"""Verify that a packaged default seed bundle exactly matches the source."""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_files(root: Path) -> dict[str, str]:
    if not root.is_dir():
        raise ValueError(f"missing seed directory: {root}")
    return {
        path.relative_to(root).as_posix(): sha256_file(path)
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--packaged", required=True, type=Path)
    args = parser.parse_args()

    try:
        source_files = collect_files(args.source)
        packaged_files = collect_files(args.packaged)
    except ValueError as error:
        print(f"seed bundle verification failed: {error}", file=sys.stderr)
        return 1

    missing = sorted(source_files.keys() - packaged_files.keys())
    unexpected = sorted(packaged_files.keys() - source_files.keys())
    mismatched = sorted(
        name
        for name in source_files.keys() & packaged_files.keys()
        if source_files[name] != packaged_files[name]
    )
    if missing or unexpected or mismatched:
        for label, paths in (
            ("missing", missing),
            ("unexpected", unexpected),
            ("mismatched", mismatched),
        ):
            for path in paths:
                print(f"seed bundle {label}: {path}", file=sys.stderr)
        return 1

    required = {"MANIFEST.json", "seed.version"}
    if not required.issubset(source_files) or not any(
        path.startswith("skills/") for path in source_files
    ):
        print("source seed bundle is incomplete", file=sys.stderr)
        return 1

    print(
        f"seed bundle OK: {len(source_files)} files in {args.packaged}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
