#!/usr/bin/env python3
"""R12: report source sizes and enforce a per-file, non-increasing baseline."""
from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "refactor"))
from layout import LayerPolicy, load_policy
from repo_files import SOURCE_SUFFIXES, byte_lines, emit_json, repo_root, tracked_files


def read_baseline(path: Path) -> dict[str, int]:
    if not path.exists():
        return {}
    return {parts[0]: int(parts[1]) for line in path.read_text(encoding="utf-8").splitlines() if line and not line.startswith("#") for parts in [line.split("\t")]}


def inspect(root: Path, files: list[str], policy: LayerPolicy, baseline: dict[str, int], limit: int = 1000) -> dict:
    rows, findings, exemptions = [], [], []
    canonical_baseline = {policy.canonical(path): value for path, value in baseline.items()}
    for path in files:
        if not path.startswith("src/") or Path(path).suffix not in SOURCE_SUFFIXES:
            continue
        canonical = policy.canonical(path)
        lines = len(byte_lines((root / path).read_bytes()))
        exempt = next((r for r in policy.rows if r["kind"] == "exempt" and "R12" in r["rule"].split(",") and (fnmatchcase(canonical, r["path"]) or fnmatchcase(path, r["path"]))), None)
        if exempt:
            exemptions.append({"file": path, "canonical": canonical, "lines": lines, "reason": exempt["note"]})
            continue
        allowed = canonical_baseline.get(canonical, limit)
        rows.append({"file": path, "canonical": canonical, "lines": lines, "allowed": allowed})
        if lines > allowed:
            findings.append({"rule": "R12", "file": path, "line": 1, "message": f"{lines} lines exceeds {allowed}", "lines": lines, "allowed": allowed})
    return {"schema": 1, "limit": limit, "oversized_files": sum(r["lines"] > limit for r in rows), "files": rows, "exemptions": exemptions, "findings": findings}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--baseline", default="scripts/layers/size_baseline.txt")
    parser.add_argument("--write-baseline", action="store_true", help="explicitly capture current oversized source files")
    parser.add_argument("--limit", type=int, default=1000)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()
    root = repo_root(args.repo)
    baseline = root / args.baseline
    report = inspect(root, tracked_files(root), load_policy(root), read_baseline(baseline), args.limit)
    if args.write_baseline:
        content = "# R12 baseline: canonical_path\tmaximum_lines. Lower limits when files shrink.\n# src/apps/web/**: user constraint exemption (D21). external/stb/**: third-party exemption.\n"
        content += "".join(f"{r['canonical']}\t{r['lines']}\n" for r in sorted(report["files"], key=lambda r: r["canonical"]) if r["lines"] > args.limit)
        baseline.write_bytes(content.encode("utf-8"))
    emit_json(report, args.output)
    return int(args.strict and bool(report["findings"]))


if __name__ == "__main__":
    raise SystemExit(main())
