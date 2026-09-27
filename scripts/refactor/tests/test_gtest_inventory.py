"""gtest_inventory.py 的解析单测(P0-07)。

场景与期望:
- CTest `--output-junit` 的报告里,带 `<skipped>` 的条目进 SKIP 清单(不算执行),带 `<failure>`
  的条目同时进 executed 与 failures(崩溃 / 超时也是 failure),status="run" 的普通条目只进 executed,
  status="notrun" 且无子元素的条目两边都不进;
- gtest 自己的 XML 解析保持原语义(status="run" 计执行,`<skipped>` 计 SKIP);
- 运行日志写到 `<output 同目录>/<stem>-run.log`,父目录不存在时自动创建。
回归时的表现:macOS 上单进程 gtest 崩溃或挂起后整份清单为空,或者 ctest 模式下把 SKIP 误计成失败。
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gtest_inventory  # noqa: E402

JUNIT = """<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="(empty)" tests="5" failures="2" disabled="0" skipped="1">
  <testcase name="Suite.Passes" classname="" time="0.01" status="run"/>
  <testcase name="Suite.Fails" classname="" time="0.02" status="run"><failure message="Failed"/></testcase>
  <testcase name="Suite.Crashes" classname="" time="0.03" status="run"><failure message="Exception: SegFault"/></testcase>
  <testcase name="Suite.Skips" classname="" time="0.00" status="notrun"><skipped message="SKIP_REGULAR_EXPRESSION_MATCHED"/></testcase>
  <testcase name="Suite.NeverRan" classname="" time="0.00" status="notrun"/>
</testsuite>
"""

GTEST_XML = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="3" failures="1" disabled="0" errors="0">
  <testsuite name="Suite" tests="3" failures="1" disabled="0" skipped="1">
    <testcase name="Passes" status="run" result="completed" classname="Suite"/>
    <testcase name="Fails" status="run" result="completed" classname="Suite"><failure message="boom"/></testcase>
    <testcase name="Skips" status="run" result="skipped" classname="Suite"><skipped message="not here"/></testcase>
  </testsuite>
</testsuites>
"""


class GtestInventoryParsingTest(unittest.TestCase):
    def test_junit_classifies_run_failure_skip_and_notrun(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "junit.xml"
            path.write_text(JUNIT, encoding="utf-8")
            result = gtest_inventory.parse_junit(path)
        self.assertEqual(result["executed"], ["Suite.Crashes", "Suite.Fails", "Suite.Passes"])
        self.assertEqual([r["name"] for r in result["skipped"]], ["Suite.Skips"])
        self.assertEqual([(r["name"], r["message"]) for r in result["failures"]],
                         [("Suite.Crashes", "Exception: SegFault"), ("Suite.Fails", "Failed")])

    def test_gtest_xml_keeps_original_semantics(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "results.xml"
            path.write_text(GTEST_XML, encoding="utf-8")
            result = gtest_inventory.parse_xml(path)
        self.assertEqual(result["executed"], ["Suite.Fails", "Suite.Passes", "Suite.Skips"])
        self.assertEqual([r["name"] for r in result["skipped"]], ["Suite.Skips"])
        self.assertEqual([r["name"] for r in result["failures"]], ["Suite.Fails"])

    def test_run_log_is_written_next_to_output_and_creates_parents(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "nested" / "gtest.json"
            name = gtest_inventory.write_run_log(str(output), b"out", b"err")
            self.assertEqual(name, "gtest-run.log")
            self.assertEqual((output.parent / name).read_bytes(), b"out\n--- stderr ---\nerr")
        self.assertIsNone(gtest_inventory.write_run_log(None, b"", b""))


if __name__ == "__main__":
    unittest.main()
