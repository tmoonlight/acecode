#!/usr/bin/env python3
"""
Validate and package an ACECode expert directory as a ZIP archive.

Adapted from WorkBuddy expert-manager (Apache-2.0).
"""

from __future__ import annotations

import argparse
import zipfile
from pathlib import Path

from validate_expert import get_expert_root, validate_expert


SKIP_DIRS = {".git", "__pycache__", "node_modules"}
SKIP_FILES = {".DS_Store", "Thumbs.db", ".gitkeep"}


def should_include(path: Path, package: Path, zip_path: Path) -> bool:
    if path.resolve() == zip_path.resolve():
        return False
    relative = path.relative_to(package)
    if any(part in SKIP_DIRS for part in relative.parts):
        return False
    if path.name in SKIP_FILES:
        return False
    return path.is_file()


def package_expert(
    expert_dir: Path,
    output_dir: Path,
    expert_root: Path,
    require_installed: bool = True,
) -> Path | None:
    result = validate_expert(
        expert_dir, expert_root=expert_root, require_installed=require_installed
    )
    print(result.summary())
    if not result.is_valid:
        return None

    package = expert_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    zip_path = (output_dir / f"{package.name}.zip").resolve()
    files = [
        item
        for item in sorted(package.rglob("*"))
        if should_include(item, package, zip_path)
    ]

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for file_path in files:
            relative = file_path.relative_to(package)
            archive.write(file_path, Path(package.name) / relative)

    print(f"PACKAGED: {len(files)} files -> {zip_path}")
    return zip_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expert_dir")
    parser.add_argument("output_dir", nargs="?", default=".")
    parser.add_argument("--expert-root", default=str(get_expert_root()))
    parser.add_argument("--allow-outside-root", action="store_true")
    args = parser.parse_args()

    created = package_expert(
        Path(args.expert_dir).expanduser().resolve(),
        Path(args.output_dir).expanduser().resolve(),
        Path(args.expert_root).expanduser().resolve(),
        require_installed=not args.allow_outside_root,
    )
    return 0 if created else 1


if __name__ == "__main__":
    raise SystemExit(main())
