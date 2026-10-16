#!/usr/bin/env python3
"""Reject incomplete or ambiguous full releases before publishing any assets."""

import argparse
from collections import defaultdict
from pathlib import Path
import re


def required_asset_names(version):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", version):
        raise ValueError("Invalid release version")
    names = [f"acecode-windows-{arch}.zip" for arch in ("x64", "arm64")]
    names += [f"acecode-linux-{arch}.tar.gz" for arch in ("x64", "arm64", "armv7")]
    # Deepin ARMv7 stopped shipping after 1.0.7; a stray archive is rejected as unexpected.
    names += [f"acecode-linux-deepin-{arch}.tar.gz" for arch in ("x64", "arm64")]
    for arch in ("x64", "arm64"):
        names += [f"acecode-macos-{arch}.tar.gz",
                  f"acecode-{version}-linux-{arch}-update.zip",
                  f"ACECode-{version}-macos-{arch}-update.zip",
                  f"ACECode-{version}-macos-{arch}.dmg",
                  f"ACECode-{version}-macos-{arch}.pkg"]
    return sorted(names)


def verify_release_assets(directory, version):
    expected = required_asset_names(version)
    found = defaultdict(list)
    for path in Path(directory).rglob("*"):
        if not path.name.lower().endswith((".tar.gz", ".zip", ".pkg", ".dmg")):
            continue
        if path.is_symlink():
            raise ValueError(f"Release assets must not be symlinks: {path}")
        if path.is_file():
            found[path.name].append(path)
    errors = []
    selected = []
    for name in expected:
        matches = found.get(name, [])
        if len(matches) != 1:
            errors.append(f"Expected exactly one {name}; found {len(matches)}")
        elif matches[0].stat().st_size == 0:
            errors.append(f"Empty release asset: {name}")
        else:
            selected.append(matches[0])
    for name in sorted(found.keys() - set(expected)):
        errors.append(f"Unexpected release asset: {name}")
    if errors:
        raise ValueError("\n".join(errors))
    return selected


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("version")
    args = parser.parse_args()
    try:
        assets = verify_release_assets(args.directory, args.version)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Verified {len(assets)} required release assets, including Deepin and both macOS PKGs.")
