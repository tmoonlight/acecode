"""run_fast_tests 的纯逻辑:过滤串、清单分组、结果合并与基线对照。

不启动真实的单测二进制;gtest 的 XML 用最小样例写到临时目录里解析。
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import run_fast_tests as rft  # noqa: E402

INVENTORY = [
    "AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn",
    "AgentLoopTermination.TextOnlyReplyEndsTurn",
    "DesktopSingleInstance.FirstAcquireSucceeds",
    "HookAgentLoop.DispatchesAssistantCompletedAfterTextMessageCommit",
    "SystemPromptTest.EnvironmentIncludesCwdLine",
    "SessionChannelBinderIntegration.BindRebindOffLifecycle",
    "TextFileBufferTest.ReadsUtf8",
]

GTEST_XML = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="3" failures="1" name="AllTests">
  <testsuite name="{suite}" tests="3" failures="1">
    <testcase name="Passes" status="run" result="completed" classname="{suite}"/>
    <testcase name="Fails" status="run" result="completed" classname="{suite}"><failure message="boom"/></testcase>
    <testcase name="Skips" status="run" result="skipped" classname="{suite}"><skipped message="no env"/></testcase>
  </testsuite>
</testsuites>
"""


class ShardFilterTest(unittest.TestCase):
    def test_fast_profile_excludes_slow_and_serial_suites(self):
        # 触发场景:快速档、没有正向过滤。期望:正向部分是 *,负向部分同时含慢套件与串行套件,
        # 这样分片进程既不会跑 139 秒的 channel binder 集成测试,也不会和串行阶段抢命名互斥体。
        text = rft.build_shard_filter("fast", None, ["DesktopSingleInstance"])
        self.assertTrue(text.startswith("*-"))
        self.assertIn("SessionChannelBinderIntegration.*", text)
        self.assertIn("DesktopSingleInstance.*", text)

    def test_full_profile_only_excludes_serial_suites_and_keeps_positive_filter(self):
        # 触发场景:全量档带正向过滤。期望:慢套件不再被排除,只剩串行套件;正向模式原样保留。
        text = rft.build_shard_filter("full", "HookAgentLoop.*:TextFileBufferTest.*", ["DesktopSingleInstance"])
        self.assertEqual("HookAgentLoop.*:TextFileBufferTest.*-DesktopSingleInstance.*", text)
        self.assertNotIn("WebServerHttp", text)


class PartitionTest(unittest.TestCase):
    def test_fast_profile_reports_not_run_and_rescues_guards(self):
        # 触发场景:快速档跑完整清单。期望:慢套件里的普通用例进 not_run(如实记录,不冒充 SKIP),
        # 同一慢套件里的守护测试进 guards 单独补跑,串行套件进 serial,其余进分片;
        # 既在慢套件表又在串行表里的 SessionChannelBinderIntegration 按排除处理,不能借串行阶段溜回来。
        parts = rft.partition_inventory(INVENTORY, "fast", None, rft.SERIAL_SUITES, rft.INVARIANT_GUARDS)
        self.assertEqual(["AgentLoopTermination.TextOnlyReplyEndsTurn",
                          "HookAgentLoop.DispatchesAssistantCompletedAfterTextMessageCommit",
                          "SessionChannelBinderIntegration.BindRebindOffLifecycle"], parts["not_run"])
        self.assertEqual(["AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn"], parts["guards"])
        self.assertEqual(["DesktopSingleInstance.FirstAcquireSucceeds"], parts["serial"])
        self.assertEqual(["SystemPromptTest.EnvironmentIncludesCwdLine",
                          "TextFileBufferTest.ReadsUtf8"], parts["sharded"])

    def test_full_profile_has_nothing_not_run(self):
        # 触发场景:全量档。期望:not_run 为空,守护测试留在分片里(不重复跑);串行套件单独跑,
        # 其中 channel binder 集成测试在全量档不再被排除,但因为绑定固定端口 286xx 必须进串行阶段
        # (回归表现:6 个分片同时跑它时 10 条用例报 port already in use)。
        parts = rft.partition_inventory(INVENTORY, "full", None, rft.SERIAL_SUITES, rft.INVARIANT_GUARDS)
        self.assertEqual([], parts["not_run"])
        self.assertEqual([], parts["guards"])
        self.assertEqual(["DesktopSingleInstance.FirstAcquireSucceeds",
                          "SessionChannelBinderIntegration.BindRebindOffLifecycle"], parts["serial"])
        self.assertEqual(5, len(parts["sharded"]))

    def test_positive_filter_uses_gtest_wildcards_and_still_reruns_guards(self):
        # 触发场景:快速档下 --filter 点名 hooks 套件,而 HookAgentLoop 本身在慢套件排除表里。
        # 期望:点名的用例照跑(显式过滤压过快速档排除,分片过滤串里也不再带慢套件);没匹配到的
        # 守护测试仍列入 guards(改动再小也要守住 prompt cache 前缀不变量);其它没匹配的不算 not_run。
        self.assertEqual("HookAgentLoop.*-DesktopSingleInstance.*",
                         rft.build_shard_filter("fast", "HookAgentLoop.*", ["DesktopSingleInstance"]))
        parts = rft.partition_inventory(INVENTORY, "fast", "HookAgentLoop.*", rft.SERIAL_SUITES, rft.INVARIANT_GUARDS)
        self.assertEqual(["HookAgentLoop.DispatchesAssistantCompletedAfterTextMessageCommit"], parts["sharded"])
        self.assertEqual(["AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn"], parts["guards"])
        self.assertEqual([], parts["not_run"])
        self.assertEqual([], parts["serial"])


class MergeAndBaselineTest(unittest.TestCase):
    def test_merge_dedupes_and_baseline_only_judges_executed_tests(self):
        # 触发场景:两个分片各产出一份 XML;基线里 WebServerHttp(快速档没跑)有一条 SKIP 和一条失败。
        # 期望:合并后 executed / skipped / failures 去重;对照时「没跑」的用例不算 skipped_removed
        # 也不算 failures_fixed,而是进 failures_not_rerun —— 回归表现:早先按全集相减会把
        # 没运行的慢套件报成「SKIP 消失」,记录看起来像修好了什么。
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            for index, suite in enumerate(("Alpha", "Beta")):
                path = Path(tmp) / f"shard-{index}.xml"
                path.write_text(GTEST_XML.format(suite=suite), encoding="utf-8")
                paths.append(path)
            paths.append(paths[0])  # 同一份重复合并,结果不能翻倍
            actual = rft.merge_results(paths)
            self.assertEqual(6, len(actual["executed"]))
            self.assertEqual(["Alpha.Skips", "Beta.Skips"], [row["name"] for row in actual["skipped"]])
            self.assertEqual(["Alpha.Fails", "Beta.Fails"], [row["name"] for row in actual["failures"]])

            report = {"tests": ["Alpha.Passes", "Alpha.Fails", "Alpha.Skips", "Beta.Passes", "Beta.Fails",
                                "Beta.Skips", "WebServerHttp.Slow", "WebServerHttp.Skipped", "WebServerHttp.Broken"],
                      "actual_results": actual}
            baseline = {"tests": report["tests"] + ["Gone.Test"],
                        "actual_results": {"executed": report["tests"],
                                           "skipped": [{"name": "WebServerHttp.Skipped", "reason": ""},
                                                       {"name": "Alpha.Skips", "reason": ""}],
                                           "failures": [{"name": "Alpha.Fails", "message": ""},
                                                        {"name": "WebServerHttp.Broken", "message": ""}]}}
            diff = rft.compare_with_baseline(report, baseline)
            self.assertEqual(["Gone.Test"], diff["tests_removed"])
            self.assertEqual([], diff["tests_added"])
            self.assertEqual(["Beta.Fails"], diff["failures_new"])
            self.assertEqual([], diff["failures_fixed"])
            self.assertEqual(["WebServerHttp.Broken"], diff["failures_not_rerun"])
            self.assertEqual(["Beta.Skips"], diff["skipped_added"])
            self.assertEqual([], diff["skipped_removed"])
            json.dumps(diff)  # 报告必须能直接序列化进记录


if __name__ == "__main__":
    unittest.main()
