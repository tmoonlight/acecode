#!/usr/bin/env python3
"""Reject incomplete or ambiguous full releases before publishing any assets."""

import argparse
from collections import defaultdict
from pathlib import Path
import re


# GitHub v1.0.17 asset sizes. A 50% ceiling allows the v1.0.18 artwork and
# refreshed catalog while catching accidental debug/unstripped runtime bundles.
# Change these intentionally with measured release evidence, never in CI itself.
BASELINE_ASSET_BYTES = {
    "acecode-windows-x64.zip": 21359678,
    "acecode-windows-arm64.zip": 21238037,
    "acecode-linux-x64.tar.gz": 19858984,
    "acecode-linux-arm64.tar.gz": 20310537,
    "acecode-linux-armv7.tar.gz": 14344144,
    "acecode-linux-deepin-x64.tar.gz": 18853288,
    "acecode-linux-deepin-arm64.tar.gz": 19229473,
    "acecode-macos-x64.tar.gz": 41518739,
    "acecode-macos-arm64.tar.gz": 40667249,
    "acecode-{version}-linux-x64-update.zip": 19931885,
    "acecode-{version}-linux-arm64-update.zip": 20389037,
    "ACECode-{version}-macos-x64-update.zip": 25098186,
    "ACECode-{version}-macos-arm64-update.zip": 24752901,
    "ACECode-{version}-macos-x64.dmg": 28476094,
    "ACECode-{version}-macos-arm64.dmg": 27797247,
    "ACECode-{version}-macos-x64.pkg": 25023400,
    "ACECode-{version}-macos-arm64.pkg": 24674956,
}


def asset_size_limits(version):
    return {name.format(version=version): size * 3 // 2
            for name, size in BASELINE_ASSET_BYTES.items()}


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
    limits = asset_size_limits(version)
    if set(limits) != set(expected):
        raise ValueError("Every release asset requires an explicit size budget")
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
        elif matches[0].stat().st_size > limits[name]:
            errors.append(f"Release asset exceeds size budget: {name}: "
                          f"{matches[0].stat().st_size} > {limits[name]} bytes")
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
