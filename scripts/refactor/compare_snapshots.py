#!/usr/bin/env python3
"""Compare two target snapshots (and optionally two gtest inventories) against the G0 rules.

P0-07 / design.md D23:P0 之后的目标快照与原始 G0 逐元组比较时,只允许两类差异:
- 「移除」的元组,其 source 必须是 `src_layout_map.tsv` 里 `delete` 行登记的 P0 授权删除
  文件,或者是这些文件在消费目标里的生成对象(`@build/CMakeFiles/<t>.dir/<file>.obj|.o`);
- 「新增」的元组,其 source 必须是 `--allowed-addition` 显式列出的 P0 新增生产文件
  (含其生成对象),或者是 `acecode_unit_tests` 下 `tests/` 的新测试源文件。
target 集合本身不允许增减。任何其它差异都是 unexpected,退出码 1。

gtest 清单的比较只报告(新增 / 删除的用例名、SKIP 与失败的变化),不阻断:P0 明确
允许新增测试,失败以各自记录为准。
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

from cmake_target_snapshot import compare
from layout import read_tsv
from repo_files import emit_json

OBJECT_RE = re.compile(r"^@build/CMakeFiles/[^/]+\.dir/(.+?)\.(?:obj|o)$")


def read_delete_rows(map_path: Path) -> set[str]:
    # 与 layout.py 同一份读取逻辑:跳过 # 注释行,按表头取列。
    return {row["old_path"].strip() for row in read_tsv(map_path) if row.get("kind") == "delete"}


def underlying_source(source: str) -> str:
    match = OBJECT_RE.match(source)
    return match.group(1) if match else source


def classify(diff: dict, deleted: set[str], additions: set[str]) -> dict:
    authorized_removed, unexpected_removed = [], []
    for row in diff["tuples"]["removed"]:
        (authorized_removed if underlying_source(row["source"]) in deleted else unexpected_removed).append(row)
    authorized_added, unexpected_added = [], []
    for row in diff["tuples"]["added"]:
        source = underlying_source(row["source"])
        test_source = row["target"] == "acecode_unit_tests" and source.startswith("tests/")
        (authorized_added if source in additions or test_source else unexpected_added).append(row)
    return {
        "targets_added": diff["targets"]["added"],
        "targets_removed": diff["targets"]["removed"],
        "authorized_removed": sorted({underlying_source(r["source"]) for r in authorized_removed}),
        "authorized_removed_tuples": len(authorized_removed),
        "authorized_added": sorted({underlying_source(r["source"]) for r in authorized_added}),
        "authorized_added_tuples": len(authorized_added),
        "unexpected_removed": unexpected_removed,
        "unexpected_added": unexpected_added,
    }


def compare_gtest(before: dict, after: dict) -> dict:
    def names(report: dict, key: str) -> set[str]:
        actual = report.get("actual_results") or {}
        rows = actual.get(key) or []
        return {row["name"] if isinstance(row, dict) else row for row in rows}

    old_tests, new_tests = set(before["tests"]), set(after["tests"])
    return {
        "tests_before": len(old_tests),
        "tests_after": len(new_tests),
        "tests_added": sorted(new_tests - old_tests),
        "tests_removed": sorted(old_tests - new_tests),
        "skipped_before": sorted(names(before, "skipped")),
        "skipped_after": sorted(names(after, "skipped")),
        "skipped_added": sorted(names(after, "skipped") - names(before, "skipped")),
        "skipped_removed": sorted(names(before, "skipped") - names(after, "skipped")),
        "failures_before": sorted(names(before, "failures")),
        "failures_after": sorted(names(after, "failures")),
        "ctest_before": len(before.get("ctest_names") or []),
        "ctest_after": len(after.get("ctest_names") or []),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, help="baseline targets.json")
    parser.add_argument("--after", required=True, help="current targets.json")
    parser.add_argument("--map", default=str(Path(__file__).with_name("src_layout_map.tsv")))
    parser.add_argument("--allowed-addition", action="append", default=[], help="P0 authorized new production source")
    parser.add_argument("--gtest-before")
    parser.add_argument("--gtest-after")
    parser.add_argument("--output")
    args = parser.parse_args()
    before = json.loads(Path(args.before).read_text(encoding="utf-8"))
    after = json.loads(Path(args.after).read_text(encoding="utf-8"))
    report = {
        "schema": 1,
        "before_targets": len(before["targets"]),
        "after_targets": len(after["targets"]),
        "before_tuples": len(before["tuples"]),
        "after_tuples": len(after["tuples"]),
        "targets": classify(compare(before, after), read_delete_rows(Path(args.map)), set(args.allowed_addition)),
    }
    if args.gtest_before and args.gtest_after:
        report["gtest"] = compare_gtest(
            json.loads(Path(args.gtest_before).read_text(encoding="utf-8")),
            json.loads(Path(args.gtest_after).read_text(encoding="utf-8")))
    targets = report["targets"]
    report["unexpected"] = bool(targets["targets_added"] or targets["targets_removed"] or
                                targets["unexpected_removed"] or targets["unexpected_added"])
    emit_json(report, args.output)
    return int(report["unexpected"])


if __name__ == "__main__":
    raise SystemExit(main())
