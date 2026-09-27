#!/usr/bin/env python3
"""Check source/test paths in tracked current documentation against the index."""
from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
from pathlib import Path
import re

from repo_files import emit_json, repo_root, tracked_files

PATH = re.compile(r"(?<![A-Za-z0-9_./\\-])(?:src|tests)/[A-Za-z0-9_./{}*?,+\-]+")


def expand_braces(value: str) -> list[str]:
    match = re.search(r"\{([^{}]+)\}", value)
    if not match:
        return [value]
    return [item for part in match[1].split(",") for item in expand_braces(value[:match.start()] + part + value[match.end():])]


def inspect(root: Path, files: list[str]) -> dict:
    known = set(files)
    for path in files:
        known.update(str(parent).replace("\\", "/") for parent in Path(path).parents if str(parent) != ".")
    findings, checked, placeholders = [], 0, []
    for path in files:
        if path.startswith("openspec/"):
            continue  # Historical plans and archived specs deliberately retain old paths.
        if not (path.endswith(".md") or path.startswith("docs/") and Path(path).suffix in {".py", ".js", ".json", ".html"}):
            continue
        if path.startswith(("external/", "web/node_modules/")):
            continue
        data = (root / path).read_text(encoding="utf-8", errors="replace")
        for match in PATH.finditer(data):
            raw = match.group().rstrip(".,")
            line = data.count("\n", 0, match.start()) + 1
            if "..." in raw or raw in ("src/", "tests/"):
                placeholders.append({"file": path, "line": line, "path": raw})
                continue
            for target in expand_braces(raw):
                target = target.rstrip("/")
                checked += 1
                if not any(fnmatchcase(candidate, target) for candidate in known):
                    findings.append({"file": path, "line": line, "path": target, "message": "documentation path is not tracked"})
    return {"schema": 1, "checked": checked, "findings": findings, "placeholders": placeholders, "excluded": ["openspec/** (historical paths are retained by the migration design)", "external/** (third-party)"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()
    root = repo_root(args.repo)
    report = inspect(root, tracked_files(root))
    emit_json(report, args.output)
    return int(args.strict and bool(report["findings"]))


if __name__ == "__main__":
    raise SystemExit(main())
