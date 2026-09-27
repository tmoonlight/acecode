"""compare_snapshots.py 的单测(P0-07 / D23 的 G0 对照规则)。

场景与期望:
- 授权删除文件本身及其在消费目标里的生成对象被移除 → 记为 authorized,不算 unexpected;
- 未登记的文件被移除、任何 target 增减 → unexpected,退出码 1;
- 新增的 tests/ 用例源与显式 --allowed-addition 的生产文件 → authorized;其它新增 → unexpected;
- gtest 清单差异只报告(新增 / 删除用例名、SKIP 变化),不影响退出码。
回归时的表现:P0-08 删掉的 20 个文件会把 P0 验收当成快照不一致而阻断,或者反过来,
任何被误删的编译单元都能悄悄通过 G0 对照。
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import compare_snapshots  # noqa: E402


def tuple_row(target: str, source: str) -> dict:
    return {"configuration": "Release", "target": target, "source": source, "language": "CXX",
            "defines": [], "compile_options": [], "generated": source.startswith("@build/")}


def snapshot(targets: list[str], tuples: list[dict]) -> dict:
    return {"schema": 1,
            "targets": [{"configuration": "Release", "name": t, "type": "EXECUTABLE", "dependencies": []} for t in targets],
            "tuples": tuples}


class CompareSnapshotsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.map_path = self.root / "map.tsv"
        self.map_path.write_text(
            "# comment line must be skipped\n"
            "old_path\tnew_path\tphase\tkind\tnote\n"
            "src/daemon/supervisor.cpp\t\tP0-08\tdelete\t-\n"
            "src/tui/foo.cpp\tsrc/apps/tui/foo.cpp\tP3\tmove\t-\n",
            encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_tool(self, before: dict, after: dict, *extra: str) -> tuple[int, dict]:
        before_path, after_path, out = self.root / "before.json", self.root / "after.json", self.root / "out.json"
        before_path.write_text(json.dumps(before), encoding="utf-8")
        after_path.write_text(json.dumps(after), encoding="utf-8")
        result = subprocess.run([sys.executable, str(TOOLS / "compare_snapshots.py"), "--before", str(before_path),
                                 "--after", str(after_path), "--map", str(self.map_path), "--output", str(out), *extra],
                                capture_output=True, text=True, encoding="utf-8", cwd=str(TOOLS))
        self.assertTrue(out.exists(), result.stderr)
        return result.returncode, json.loads(out.read_text(encoding="utf-8"))

    def test_delete_rows_skip_comments_and_only_take_delete_kind(self) -> None:
        # 场景:映射表首行是 # 注释,且含 move 行。期望:只有 delete 行进入授权删除集合。
        self.assertEqual(compare_snapshots.read_delete_rows(self.map_path), {"src/daemon/supervisor.cpp"})

    def test_authorized_deletion_and_its_object_are_not_unexpected(self) -> None:
        # 场景:P0-08 删除 supervisor.cpp,其在 acecode 里的生成对象随之消失。
        # 期望:两条移除都记为 authorized,退出码 0。
        before = snapshot(["acecode", "acecode_testable"], [
            tuple_row("acecode_testable", "src/daemon/supervisor.cpp"),
            tuple_row("acecode", "@build/CMakeFiles/acecode_testable.dir/src/daemon/supervisor.cpp.obj"),
            tuple_row("acecode", "src/main.cpp")])
        after = snapshot(["acecode", "acecode_testable"], [tuple_row("acecode", "src/main.cpp")])
        code, report = self.run_tool(before, after)
        self.assertEqual(code, 0, report)
        self.assertFalse(report["unexpected"])
        self.assertEqual(report["targets"]["authorized_removed"], ["src/daemon/supervisor.cpp"])
        self.assertEqual(report["targets"]["authorized_removed_tuples"], 2)

    def test_unregistered_deletion_is_unexpected(self) -> None:
        # 场景:一个没有登记为 delete 的文件从目标里消失。期望:unexpected,退出码 1。
        before = snapshot(["acecode"], [tuple_row("acecode", "src/main.cpp"), tuple_row("acecode", "src/tui/foo.cpp")])
        after = snapshot(["acecode"], [tuple_row("acecode", "src/main.cpp")])
        code, report = self.run_tool(before, after)
        self.assertEqual(code, 1)
        self.assertEqual([r["source"] for r in report["targets"]["unexpected_removed"]], ["src/tui/foo.cpp"])

    def test_target_set_changes_are_unexpected_even_without_tuple_changes(self) -> None:
        # 场景:多出一个 target。期望:unexpected,即便元组没变。
        before = snapshot(["acecode"], [tuple_row("acecode", "src/main.cpp")])
        after = snapshot(["acecode", "extra"], [tuple_row("acecode", "src/main.cpp")])
        code, report = self.run_tool(before, after)
        self.assertEqual(code, 1)
        self.assertEqual([t["name"] for t in report["targets"]["targets_added"]], ["extra"])

    def test_new_tests_and_listed_production_files_are_authorized_additions(self) -> None:
        # 场景:P0-11 新增测试源,P2-01 新增 abandonable_call.cpp 及其生成对象;另有一个
        # 未登记的新生产文件。期望:前两类 authorized,第三类 unexpected。
        before = snapshot(["acecode", "acecode_testable", "acecode_unit_tests"], [tuple_row("acecode", "src/main.cpp")])
        after = snapshot(["acecode", "acecode_testable", "acecode_unit_tests"], [
            tuple_row("acecode", "src/main.cpp"),
            tuple_row("acecode_unit_tests", "tests/agent_loop/characterization_lease_and_clock_test.cpp"),
            tuple_row("acecode_testable", "src/utils/abandonable_call.cpp"),
            tuple_row("acecode", "@build/CMakeFiles/acecode_testable.dir/src/utils/abandonable_call.cpp.obj"),
            tuple_row("acecode_testable", "src/utils/sneaky.cpp")])
        code, report = self.run_tool(before, after, "--allowed-addition", "src/utils/abandonable_call.cpp")
        self.assertEqual(code, 1)
        self.assertEqual(report["targets"]["authorized_added"],
                         ["src/utils/abandonable_call.cpp", "tests/agent_loop/characterization_lease_and_clock_test.cpp"])
        self.assertEqual(report["targets"]["authorized_added_tuples"], 3)
        self.assertEqual([r["source"] for r in report["targets"]["unexpected_added"]], ["src/utils/sneaky.cpp"])

    def test_gtest_section_is_informational(self) -> None:
        # 场景:新清单多了一个用例、少了一个 SKIP。期望:差异写进报告但退出码仍由目标决定。
        gb, ga = self.root / "gb.json", self.root / "ga.json"
        gb.write_text(json.dumps({"tests": ["A.one", "A.two"], "actual_results": {"executed": ["A.one"], "skipped": [{"name": "A.two", "reason": "x"}], "failures": []}, "ctest_names": ["A.one", "A.two"]}), encoding="utf-8")
        ga.write_text(json.dumps({"tests": ["A.one", "A.two", "B.new"], "actual_results": {"executed": ["A.one", "A.two", "B.new"], "skipped": [], "failures": [{"name": "B.new", "message": "boom"}]}, "ctest_names": ["A.one", "A.two", "B.new"]}), encoding="utf-8")
        before = snapshot(["acecode"], [tuple_row("acecode", "src/main.cpp")])
        code, report = self.run_tool(before, before, "--gtest-before", str(gb), "--gtest-after", str(ga))
        self.assertEqual(code, 0)
        self.assertEqual(report["gtest"]["tests_added"], ["B.new"])
        self.assertEqual(report["gtest"]["skipped_removed"], ["A.two"])
        self.assertEqual(report["gtest"]["failures_after"], ["B.new"])


if __name__ == "__main__":
    unittest.main()
