#!/usr/bin/env python3
"""Capture every CMake File API target, including EXCLUDE_FROM_ALL targets."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

from layout import LayoutMap, read_tsv
from repo_files import emit_json


def normalize_root_paths(value: str, source_root: str, build_root: str) -> str:
    """Replace only complete path-root tokens, including quoted define values."""
    value = value.replace("\\", "/")
    for root, marker in sorted(((source_root, "@source"), (build_root, "@build")), key=lambda pair: len(pair[0]), reverse=True):
        root = root.replace("\\", "/").rstrip("/")
        if not root:
            continue
        left = r"(?:(?<![A-Za-z0-9_./-])|(?<=-I)|(?<=/I))"
        right = r"(?=$|[/\s\"';,)])"
        value = re.sub(left + re.escape(root) + right, lambda _match: marker, value)
    if value.startswith("@source/"):
        value = value[8:]
    return value


def write_query(build: Path) -> None:
    query = build / ".cmake/api/v1/query/codemodel-v2"
    query.parent.mkdir(parents=True, exist_ok=True)
    query.write_bytes(b"")


def snapshot(build: Path, configuration: str | None = None, mapping: LayoutMap | None = None, reverse: bool = False) -> dict:
    reply = build / ".cmake/api/v1/reply"
    # Only the explicit CMake generated reply directory is enumerated. Repository
    # source discovery remains git ls-files in all source-rewriting tools.
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise ValueError("no File API reply; run --query, then configure CMake")
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    reference = next((obj for obj in index.get("objects", []) if obj["kind"] == "codemodel"), None)
    if reference is None:
        raise ValueError("the newest File API reply has no codemodel-v2")
    model = json.loads((reply / reference["jsonFile"]).read_text(encoding="utf-8"))
    source_root = model["paths"]["source"].replace("\\", "/").rstrip("/")
    build_root = model["paths"]["build"].replace("\\", "/").rstrip("/")

    def normalize(value: str) -> str:
        value = normalize_root_paths(value, source_root, build_root)
        if mapping:
            translated = mapping.translate(value, reverse)
            if translated is not None:
                value = translated
        return value

    targets, tuples = [], []
    configurations = [cfg for cfg in model["configurations"] if configuration is None or cfg["name"].lower() == configuration.lower()]
    if not configurations:
        raise ValueError(f"configuration {configuration!r} not found")
    for cfg in configurations:
        names = {t["id"]: t["name"] for t in cfg.get("targets", [])}
        for reference in cfg.get("targets", []):
            target = json.loads((reply / reference["jsonFile"]).read_text(encoding="utf-8"))
            targets.append({"configuration": cfg["name"], "name": target["name"], "type": target["type"], "dependencies": sorted(names.get(d["id"], d["id"]) for d in target.get("dependencies", []))})
            groups = target.get("compileGroups", [])
            for source in target.get("sources", []):
                group = groups[source["compileGroupIndex"]] if "compileGroupIndex" in source else {}
                tuples.append({"configuration": cfg["name"], "target": target["name"], "source": normalize(source["path"]), "language": group.get("language", ""), "defines": sorted(normalize(d["define"]) for d in group.get("defines", [])), "compile_options": [normalize(f["fragment"]) for f in group.get("compileCommandFragments", [])], "generated": bool(source.get("isGenerated", False))})
    return {"schema": 1, "targets": sorted(targets, key=lambda t: (t["configuration"], t["name"])), "tuples": sorted(tuples, key=lambda t: (t["configuration"], t["target"], t["source"]))}


def compare(before: dict, after: dict) -> dict:
    def differences(key: str) -> dict:
        old = {json.dumps(row, sort_keys=True) for row in before[key]}
        new = {json.dumps(row, sort_keys=True) for row in after[key]}
        return {"removed": [json.loads(row) for row in sorted(old - new)], "added": [json.loads(row) for row in sorted(new - old)]}
    return {key: differences(key) for key in ("targets", "tuples")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--query", action="store_true", help="write the query and exit; configure CMake afterwards")
    parser.add_argument("--configuration")
    parser.add_argument("--map")
    parser.add_argument("--reverse-map", action="store_true")
    parser.add_argument("--compare")
    parser.add_argument("--output")
    args = parser.parse_args()
    build = Path(args.build_dir)
    if args.query:
        write_query(build)
        return 0
    mapping = LayoutMap(read_tsv(Path(args.map))) if args.map else None
    report = snapshot(build, args.configuration, mapping, args.reverse_map)
    if args.compare:
        report["comparison"] = compare(json.loads(Path(args.compare).read_text(encoding="utf-8")), report)
    emit_json(report, args.output)
    return int(any(diff["removed"] or diff["added"] for diff in report.get("comparison", {}).values()))


if __name__ == "__main__":
    raise SystemExit(main())
