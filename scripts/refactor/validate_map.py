#!/usr/bin/env python3
"""Validate longest-prefix migration mappings and detect destination collisions."""
from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path, PurePosixPath

from layout import LayoutMap, load_policy, read_tsv
from repo_files import emit_json, repo_root, tracked_files


def inspect(files: list[str], mapping: LayoutMap, policy=None) -> dict:
    findings, dormant = [], []
    destinations, basenames = defaultdict(list), defaultdict(list)
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
        destinations[destination.casefold()].append(path)
        if path.startswith("src/"):
            basenames[PurePosixPath(destination).name.casefold()].append(destination)
            if policy and destination.startswith("src/") and destination != "src/layers.tsv" and policy.module_for(destination, False) is None:
                findings.append({"path": path, "destination": destination, "message": "destination has no registered module"})
    for destination, sources in destinations.items():
        if len(sources) > 1:
            findings.append({"destination": destination, "sources": sources, "message": "case-insensitive destination collision"})
    return {"schema": 1, "mapped_files": sum(mapping.translate(p) != p for p in files), "findings": findings, "planned_or_obsolete_rows": dormant, "duplicate_basenames": {name: sorted(set(paths)) for name, paths in sorted(basenames.items()) if len(set(paths)) > 1}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--map", default="scripts/refactor/src_layout_map.tsv")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()
    root = repo_root(args.repo)
    report = inspect(tracked_files(root), LayoutMap(read_tsv(root / args.map)), load_policy(root))
    emit_json(report, args.output)
    return int(args.strict and bool(report["findings"]))


if __name__ == "__main__":
    raise SystemExit(main())
