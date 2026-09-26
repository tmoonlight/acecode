#!/usr/bin/env python3
"""Normalize tracked C++ includes without changing any other byte."""
from __future__ import annotations

import argparse
from pathlib import Path

from layout import IncludeIndex, LayoutMap, include_matches, read_tsv
from repo_files import SOURCE_SUFFIXES, emit_json, repo_root, tracked_files, tracked_path, write_bytes_if_changed


def normalize(data: bytes, path: str, index: IncludeIndex) -> tuple[bytes, list[dict], list[dict]]:
    edits, errors = [], []
    for match in include_matches(data):
        if match[1] != b'"':
            continue
        name = match[2].decode("utf-8")
        targets = index.resolve(path, name)
        line = data.count(b"\n", 0, match.start()) + 1
        if len(targets) != 1:
            if "/" not in name and any(str(Path(t).parent) == str(Path(path).parent) for t in targets):
                # P1 explicitly preserves same-directory bare includes. R8 still
                # reports any generated-header ambiguity for P2 to resolve.
                continue
            # Third-party quoted headers are not rewritten. Relative or known
            # project paths must resolve; never guess between multiple roots.
            if targets or name.startswith(("../", "test_support/")):
                errors.append({"file": path, "line": line, "include": name, "targets": targets})
            continue
        target = targets[0]
        if target.startswith("@generated/"):
            continue
        same_directory = str(Path(target).parent) == str(Path(path).parent)
        helper = target.startswith("tests/test_support/")
        if same_directory and "/" not in name and not helper:
            continue
        canonical = index.canonical_include(target)
        if canonical != name:
            edits.append({"file": path, "line": line, "old": name, "new": canonical, "start": match.start(2), "end": match.end(2)})
    result = data
    for edit in reversed(edits):
        result = result[:edit["start"]] + edit["new"].encode("utf-8") + result[edit["end"]:]
    return result, edits, errors


def run(root: Path, check: bool = True, scope: str = "all") -> dict:
    files = tracked_files(root)
    mapping_path = root / "scripts/refactor/src_layout_map.tsv"
    aliases = LayoutMap(read_tsv(mapping_path)) if mapping_path.exists() else None
    index = IncludeIndex(files, aliases=aliases)
    changes, errors, relative_files = [], [], set()
    relative_lines = 0
    for path in files:
        if Path(path).suffix not in SOURCE_SUFFIXES or not path.startswith(("src/", "tests/")):
            continue
        if "/stb/" in path or scope != "all" and not path.startswith(scope + "/"):
            continue
        file = tracked_path(root, path, files)
        data = file.read_bytes()
        relatives = sum(b"../" in m[2] for m in include_matches(data) if m[1] == b'"')
        relative_lines += relatives
        if relatives:
            relative_files.add(path)
        updated, edits, failures = normalize(data, path, index)
        changes.extend(edits)
        errors.extend(failures)
        if not check and not failures:
            write_bytes_if_changed(file, data, updated)
    return {"schema": 1, "check": check, "scope": scope, "changed_files": len({c["file"] for c in changes}), "changed_lines": len(changes), "relative_include_lines": relative_lines, "relative_include_files": len(relative_files), "changes": [{k: v for k, v in c.items() if k not in ("start", "end")} for c in changes], "errors": errors}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--scope", choices=("all", "src", "tests"), default="all")
    parser.add_argument("--output")
    args = parser.parse_args()
    report = run(repo_root(args.repo), args.check, args.scope)
    emit_json(report, args.output)
    return int(bool(report["errors"]) or args.check and report["changed_lines"] > 0)


if __name__ == "__main__":
    raise SystemExit(main())
