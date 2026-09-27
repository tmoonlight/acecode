#!/usr/bin/env python3
"""Check that every original line has a valid, explicitly mapped destination."""
from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path

from layout import read_tsv
from repo_files import byte_lines, emit_json, git, repo_root, tracked_files


def inspect(root: Path, source_ref: str, rows: list[dict[str, str]], originals: list[str]) -> dict:
    files = set(tracked_files(root))
    grouped = defaultdict(list)
    for row in rows:
        grouped[row["old_path"]].append(row)
    findings, coverage = [], []
    for path in sorted(set(originals) | set(grouped)):
        try:
            original = byte_lines(git(root, "show", f"{source_ref}:{path}"))
        except RuntimeError as error:
            findings.append({"file": path, "message": str(error)})
            continue
        owners = [[] for _ in original]
        for index, row in enumerate(grouped[path], 1):
            try:
                start, end = int(row["old_start"]), int(row["old_end"])
                new_start, new_end = int(row["new_start"]), int(row["new_end"])
                target = row["new_path"]
                if not 1 <= start <= end <= len(original):
                    raise ValueError("original range out of bounds")
                if target not in files:
                    raise ValueError("destination is not a tracked file: " + target)
                destination = byte_lines((root / target).read_bytes())
                if not 1 <= new_start <= new_end <= len(destination):
                    raise ValueError("destination range out of bounds")
                mode = row.get("mode", "copy") or "copy"
                if mode == "copy":
                    before = [line.rstrip(b"\r\n") for line in original[start - 1:end]]
                    after = [line.rstrip(b"\r\n") for line in destination[new_start - 1:new_end]]
                    if before != after:
                        raise ValueError("copy mapping content differs")
                elif mode == "edited":
                    if not row.get("reason", "").strip():
                        raise ValueError("edited mapping requires an explicit reason")
                else:
                    raise ValueError("unknown mode; deletion cannot satisfy line coverage")
                for line in range(start - 1, end):
                    owners[line].append(index)
            except (KeyError, ValueError, OSError) as error:
                findings.append({"file": path, "mapping_row": index, "message": str(error)})
        missing = [i + 1 for i, assigned in enumerate(owners) if not assigned]
        duplicates = [i + 1 for i, assigned in enumerate(owners) if len(assigned) > 1]
        coverage.append({"file": path, "lines": len(original), "mapped": sum(bool(x) for x in owners), "missing": missing, "duplicates": duplicates})
        if missing or duplicates:
            findings.append({"file": path, "message": "line mapping is not a complete partition", "missing": missing, "duplicates": duplicates})
    return {"schema": 1, "source_ref": source_ref, "coverage": coverage, "findings": findings}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--source-ref", default="HEAD")
    parser.add_argument("--map", required=True, help="TSV old_path,old_start,old_end,new_path,new_start,new_end,mode,reason")
    parser.add_argument("--original", action="append", default=[], help="required original file; also detects files omitted entirely from the map")
    parser.add_argument("--output")
    args = parser.parse_args()
    report = inspect(repo_root(args.repo), args.source_ref, read_tsv(Path(args.map)), args.original)
    emit_json(report, args.output)
    return int(bool(report["findings"]) or not report["coverage"])


if __name__ == "__main__":
    raise SystemExit(main())
