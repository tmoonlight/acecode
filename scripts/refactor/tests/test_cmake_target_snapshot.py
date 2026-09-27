"""cmake_target_snapshot 的路径换算:P2 过渡目录也要能反查回 G0 的旧路径。"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from cmake_target_snapshot import translate_for_comparison  # noqa: E402
from layout import LayoutMap  # noqa: E402

ROWS = [
    {"old_path": "src/utils/", "new_path": "src/base/utils/", "phase": "P3", "kind": "move", "note": "-"},
    {"old_path": "src/platform/", "new_path": "src/base/platform/", "phase": "P3", "kind": "move", "note": "-"},
    {"old_path": "src/utils/clipboard.cpp", "new_path": "src/base/platform/clipboard.cpp", "phase": "P2-03", "kind": "move", "note": "-"},
    {"old_path": "tests/utils/clipboard_test.cpp", "new_path": "tests/platform/clipboard_test.cpp", "phase": "P2-03", "kind": "move", "note": "-"},
    {"old_path": "src/tui/dead.cpp", "new_path": "-", "phase": "P0-08", "kind": "delete", "note": "-"},
    {"old_path": "src/commands/compact.hpp", "new_path": "src/domain/llm/token_estimate.hpp", "phase": "P2-02", "kind": "extract", "note": "-"},
]


class TranslateForComparisonTest(unittest.TestCase):
    def setUp(self):
        self.mapping = LayoutMap(ROWS)

    def test_transition_path_maps_back_to_original(self):
        # 触发场景:P2-03 把 clipboard.cpp 搬到过渡目录 src/platform/,快照里是 src/platform/clipboard.cpp。
        # 期望:先按 P3 目录行正向到 src/base/platform/clipboard.cpp,再按精确的 P2-03 行反查回
        # src/utils/clipboard.cpp,与 G0 逐元组可比。回归表现:改动前 --reverse-map 只做一次反向查找,
        # 过渡路径没有任何 new_path 与之匹配,原样留下,导致每个搬迁文件都报「一减一增」。
        self.assertEqual(translate_for_comparison(self.mapping, "src/platform/clipboard.cpp", True), "src/utils/clipboard.cpp")

    def test_original_path_round_trips(self):
        # 触发场景:没搬过的文件在快照里仍是旧路径。期望:正向到最终路径再反向,回到自身。
        self.assertEqual(translate_for_comparison(self.mapping, "src/utils/paths.cpp", True), "src/utils/paths.cpp")
        self.assertEqual(translate_for_comparison(self.mapping, "src/utils/clipboard.cpp", True), "src/utils/clipboard.cpp")

    def test_final_path_maps_back(self):
        # 触发场景:P3 之后快照里已是最终路径。期望:直接反查到旧路径(与 P0-07 的既有行为一致)。
        self.assertEqual(translate_for_comparison(self.mapping, "src/base/platform/clipboard.cpp", True), "src/utils/clipboard.cpp")
        self.assertEqual(translate_for_comparison(self.mapping, "src/base/utils/paths.cpp", True), "src/utils/paths.cpp")

    def test_tests_mirror_and_unmapped_values(self):
        # 测试镜像行没有分组层,精确行即可反查;delete 行、extract 行与非路径文本一律不动。
        self.assertEqual(translate_for_comparison(self.mapping, "tests/platform/clipboard_test.cpp", True), "tests/utils/clipboard_test.cpp")
        self.assertEqual(translate_for_comparison(self.mapping, "src/tui/dead.cpp", True), "src/tui/dead.cpp")
        self.assertEqual(translate_for_comparison(self.mapping, "src/commands/compact.hpp", True), "src/commands/compact.hpp")
        self.assertEqual(translate_for_comparison(self.mapping, "ACECODE_FOO=1", True), "ACECODE_FOO=1")

    def test_forward_mode_and_no_mapping_are_unchanged(self):
        # 正向模式只做一次映射(P3 之后拿旧基线正向换算用);没有映射表时原样返回。
        self.assertEqual(translate_for_comparison(self.mapping, "src/utils/clipboard.cpp", False), "src/base/platform/clipboard.cpp")
        self.assertEqual(translate_for_comparison(self.mapping, "src/platform/clipboard.cpp", False), "src/base/platform/clipboard.cpp")
        self.assertEqual(translate_for_comparison(None, "src/utils/clipboard.cpp", True), "src/utils/clipboard.cpp")


if __name__ == "__main__":
    unittest.main()
