#!/usr/bin/env python3
"""Validate longest-prefix migration mappings and detect destination collisions."""
from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path, PurePosixPath
import re

from layout import IncludeIndex, LayoutMap, load_policy, read_tsv
from repo_files import emit_json, repo_root, tracked_files

# P2 forwarding header: exactly `#pragma once` plus one quoted include (design.md
# "迁移机制"). It stays at the old path until the P3-02 freeze deletes it.
FORWARDING_HEADER = re.compile(rb'\A#pragma once\r?\n[ \t]*#[ \t]*include[ \t]*"([^"\r\n]+)"[ \t]*\r?\n?\Z')


def forwarding_target(root: Path | None, path: str, mapping: LayoutMap, index: IncludeIndex) -> str | None:
    """Return the relocated file a P2 forwarding stub points at, or None.

    A stub is recognised by content, not by name: the old header holds only the
    two forwarding lines and its include resolves to exactly one other tracked
    file which the map sends to the same final destination. Both files therefore
    describe one relocated header, so the pair is not a destination collision.
    """
    if root is None or not path.startswith("src/") or not path.endswith((".hpp", ".h")):
        return None
    match = FORWARDING_HEADER.match((root / path).read_bytes())
    if not match:
        return None
    targets = index.resolve(path, match[1].decode("utf-8", "replace"))
    if len(targets) != 1 or targets[0] == path or targets[0].startswith("@generated/"):
        return None
    if mapping.translate(targets[0]) != mapping.translate(path):
        return None
    return targets[0]


def inspect(files: list[str], mapping: LayoutMap, policy=None, root: Path | None = None) -> dict:
    findings, dormant, forwarding = [], [], []
    destinations, basenames = defaultdict(list), defaultdict(list)
    index = IncludeIndex(files, aliases=mapping)
    keys = set()
    for row in mapping.rows:
        old, new, kind = row["old_path"], row["new_path"], row["kind"]
        if kind not in ("move", "delete", "extract"):
            findings.append({"path": old, "message": "unknown map kind " + kind})
        if kind != "extract" and old in keys:
            findings.append({"path": old, "message": "duplicate source mapping"})
        if kind != "extract":
            keys.add(old)
        for path in (old, new):
            if path == "-" and kind == "delete":
                continue
            if path.startswith("/") or ":" in path or "\\" in path or ".." in PurePosixPath(path).parts:
                findings.append({"path": path, "message": "map path must be repository-relative POSIX"})
        if kind == "move" and old.endswith("/") != new.endswith("/"):
            findings.append({"path": old, "message": "both directory prefixes must end in /"})
        if kind == "delete" and new != "-":
            findings.append({"path": old, "message": "delete destination must be -"})
        if not any(path == old or old.endswith("/") and path.startswith(old) for path in files):
            dormant.append({"path": old, "phase": row["phase"], "kind": kind})
    for path in files:
        destination = mapping.translate(path)
        if destination is None:
            continue
        target = forwarding_target(root, path, mapping, index)
        if target is not None:
            forwarding.append({"path": path, "target": target, "destination": destination})
            continue
        destinations[destination.casefold()].append(path)
        if path.startswith("src/"):
            basenames[PurePosixPath(destination).name.casefold()].append(destination)
            if policy and destination.startswith("src/") and destination != "src/layers.tsv" and policy.module_for(destination, False) is None:
                findings.append({"path": path, "destination": destination, "message": "destination has no registered module"})
    for destination, sources in destinations.items():
        if len(sources) > 1:
            findings.append({"destination": destination, "sources": sources, "message": "case-insensitive destination collision"})
    return {"schema": 1, "mapped_files": sum(mapping.translate(p) != p for p in files), "findings": findings, "forwarding_headers": forwarding, "planned_or_obsolete_rows": dormant, "duplicate_basenames": {name: sorted(set(paths)) for name, paths in sorted(basenames.items()) if len(set(paths)) > 1}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--map", default="scripts/refactor/src_layout_map.tsv")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()
    root = repo_root(args.repo)
    report = inspect(tracked_files(root), LayoutMap(read_tsv(root / args.map)), load_policy(root), root)
    emit_json(report, args.output)
    return int(args.strict and bool(report["findings"]))


if __name__ == "__main__":
    raise SystemExit(main())
