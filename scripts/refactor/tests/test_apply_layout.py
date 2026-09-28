"""apply_layout 的真实 Git 夹具:P2 过渡目录搬迁、P3 最终布局搬迁、M1 只有 R100、M2 改写与 blame 登记。"""
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from apply_layout import commit, git as user_git, phase_map, step_blame, step_move, step_rewrite, unfinished_rows  # noqa: E402
from layout import read_tsv  # noqa: E402
from migration_git import git, revision  # noqa: E402

MAP = (
    "old_path\tnew_path\tphase\tkind\tnote\n"
    "src/utils/b.cpp\tsrc/adapters/tool/b.cpp\tP2-08\tmove\t-\n"
    "tests/agent_loop/\ttests/agent/\tP2-08\tmove\t-\n"
    "src/utils/\tsrc/base/utils/\tP3\tmove\t-\n"
    "src/tool/\tsrc/adapters/tool/\tP3\tmove\t-\n"
)


class ApplyLayoutTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="acecode-apply-layout-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.root.mkdir()
        git(self.root, "init", "-b", "master")
        git(self.root, "config", "user.name", "apply_layout tests")
        git(self.root, "config", "user.email", "tests@example.invalid")
        git(self.root, "config", "core.autocrlf", "false")
        self.write("scripts/refactor/src_layout_map.tsv", MAP)
        self.map = self.root / "scripts/refactor/src_layout_map.tsv"
        self.write("src/utils/a.hpp", "#pragma once\n")
        self.write("src/utils/a.cpp", '#include "utils/a.hpp"\n')
        self.write("src/utils/b.cpp", '#include "utils/a.hpp"\n')
        self.write("src/tool/c.hpp", "#pragma once\n")
        self.write("src/tool/c.cpp", '#include "utils/a.hpp"\n#include "c.hpp"\n')
        self.write("tests/utils/a_test.cpp", '#include "utils/a.hpp"\n')
        self.write("tests/agent_loop/x_test.cpp", '#include "utils/a.hpp"\n')
        self.write("CMakeLists.txt", "add_library(x ${CMAKE_SOURCE_DIR}/src/utils/a.cpp ${CMAKE_SOURCE_DIR}/src/utils/b.cpp ${CMAKE_SOURCE_DIR}/src/tool/c.cpp)\n")
        self.write("tests/CMakeLists.txt", "add_executable(t utils/a_test.cpp agent_loop/x_test.cpp)\n")
        self.write("docs/guide.md", "see src/utils/b.cpp and src/tool/c.cpp; tests in tests/agent_loop/\n")
        git(self.root, "add", "--all")
        git(self.root, "commit", "-q", "-m", "base")
        self.rows = read_tsv(self.map)

    def write(self, path, text):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(text.encode("utf-8"))

    def read(self, path):
        return (self.root / path).read_text(encoding="utf-8")

    def staged(self):
        return git(self.root, "diff", "--cached", "-M100%", "--name-status").decode().strip().splitlines()

    def test_phase_map_lands_p2_rows_in_transition_directories(self):
        # 触发场景:P2-08 行的 new_path 写的是最终布局 src/adapters/tool/b.cpp。期望:过渡布局把它落到
        # src/tool/b.cpp(去掉分组层),tests/ 行原样;同一张表按 P3 阶段取时用最终路径。
        transition = phase_map(self.rows, ["P2-08"], "transition")
        self.assertEqual("src/tool/b.cpp", transition.translate("src/utils/b.cpp"))
        self.assertEqual("tests/agent/x_test.cpp", transition.translate("tests/agent_loop/x_test.cpp"))
        self.assertEqual("src/utils/a.cpp", transition.translate("src/utils/a.cpp"))  # P3 行不在本阶段
        final = phase_map(self.rows, ["P3"], "final")
        self.assertEqual("src/base/utils/a.cpp", final.translate("src/utils/a.cpp"))

    def test_p3_refuses_while_p2_rows_are_unfinished_unless_rehearsing(self):
        # 触发场景:src/utils/b.cpp 还没按 P2-08 搬走就跑 P3。期望:报出未完成行、不写任何东西;
        # --allow-unfinished(演练)才继续。回归表现:跳过 P2 直接搬 P3 会让 b.cpp 落到 base/utils 而不是 adapters/tool。
        pending = unfinished_rows(self.rows, ["P3"], ["src/utils/b.cpp", "src/utils/a.cpp"])
        self.assertEqual(["src/utils/b.cpp"], [row["old_path"] for row in pending])
        report = step_move(self.root, self.rows, ["P3"], dry_run=False, allow_unfinished=False, allow_dirty=False)
        self.assertIn("error", report)
        self.assertFalse(report["applied"])
        self.assertEqual([], self.staged())
        report = step_move(self.root, self.rows, ["P3"], dry_run=True, allow_unfinished=True, allow_dirty=False)
        self.assertFalse(report["applied"])
        self.assertEqual({"src/utils/a.hpp", "src/utils/a.cpp", "src/utils/b.cpp", "src/tool/c.hpp", "src/tool/c.cpp"}, set(report["moves"]))
        self.assertEqual([], self.staged())  # dry-run 不动索引

    def test_p2_transition_move_then_rewrite_updates_cmake_docs_and_keeps_includes(self):
        # 触发场景:P2-08 搬迁 + 改写。期望:M1 暂存区只有 R100;M2 把根 CMake 的绝对路径与 tests/CMakeLists.txt
        # 的相对路径都换到新位置、文档路径换成过渡路径、include 不需要改(模块根形式仍能解析);两步提交带 [mechanical]。
        report = step_move(self.root, self.rows, ["P2-08"], dry_run=False, allow_unfinished=False, allow_dirty=False)
        self.assertTrue(report["applied"], report)
        self.assertEqual({"src/utils/b.cpp": "src/tool/b.cpp", "tests/agent_loop/x_test.cpp": "tests/agent/x_test.cpp"}, report["moves"])
        self.assertTrue(all(line.startswith("R100") for line in self.staged()), self.staged())
        self.assertIn("tests/agent_loop", report["removed_directories"])
        first = commit(self.root, "P2-08", "搬迁", "[mechanical]")
        self.assertIn("[mechanical]", git(self.root, "log", "-1", "--format=%s", first).decode())

        report = step_rewrite(self.root, self.map, self.rows, ["P2-08"], dry_run=False, design_note=False)
        self.assertTrue(report["applied"], report)
        self.assertEqual(["CMakeLists.txt", "tests/CMakeLists.txt"], report["build_files"])
        self.assertIn("${CMAKE_SOURCE_DIR}/src/tool/b.cpp", self.read("CMakeLists.txt"))
        self.assertIn("${CMAKE_SOURCE_DIR}/src/utils/a.cpp", self.read("CMakeLists.txt"))
        self.assertEqual("add_executable(t utils/a_test.cpp agent/x_test.cpp)\n", self.read("tests/CMakeLists.txt"))
        self.assertEqual("see src/tool/b.cpp and src/tool/c.cpp; tests in tests/agent/\n", self.read("docs/guide.md"))
        self.assertEqual(0, report["include_changed_lines"])
        self.assertEqual([], report["include_errors"])
        second = commit(self.root, "P2-08", "改写", "[mechanical]")
        self.assertEqual("", git(self.root, "status", "--porcelain").decode().strip())
        self.assertNotEqual(first, second)

    def test_p3_final_move_is_pure_rename_and_rewrite_changes_no_include_lines(self):
        # 触发场景:P2-08 已完成后跑 P3。期望:M1 只有 R100、不改任何内容(tests/ 不动);M2 改根 CMake 与文档,
        # include 改动为 0 行(P1 把 include 统一成模块根形式就是为了这一天);blame 登记两个提交的完整 SHA。
        step_move(self.root, self.rows, ["P2-08"], dry_run=False, allow_unfinished=False, allow_dirty=False)
        commit(self.root, "P2-08", "搬迁", "[mechanical]")
        step_rewrite(self.root, self.map, self.rows, ["P2-08"], dry_run=False, design_note=False)
        commit(self.root, "P2-08", "改写", "[mechanical]")

        report = step_move(self.root, self.rows, ["P3"], dry_run=False, allow_unfinished=False, allow_dirty=False)
        self.assertTrue(report["applied"], report)
        self.assertEqual("final", report["layout"])
        self.assertEqual({"src/utils/a.hpp": "src/base/utils/a.hpp", "src/utils/a.cpp": "src/base/utils/a.cpp",
                          "src/tool/b.cpp": "src/adapters/tool/b.cpp", "src/tool/c.hpp": "src/adapters/tool/c.hpp",
                          "src/tool/c.cpp": "src/adapters/tool/c.cpp"}, report["moves"])
        self.assertTrue(all(line.startswith("R100") for line in self.staged()), self.staged())
        self.assertTrue((self.root / "tests/utils/a_test.cpp").exists())
        m1 = commit(self.root, "P3", "M1", "[no-build]")

        report = step_rewrite(self.root, self.map, self.rows, ["P3"], dry_run=False, design_note=False)
        self.assertTrue(report["applied"], report)
        self.assertEqual(0, report["include_changed_lines"], report)
        self.assertEqual([], report["include_errors"])
        self.assertEqual("add_library(x ${CMAKE_SOURCE_DIR}/src/base/utils/a.cpp ${CMAKE_SOURCE_DIR}/src/adapters/tool/b.cpp ${CMAKE_SOURCE_DIR}/src/adapters/tool/c.cpp)\n", self.read("CMakeLists.txt"))
        self.assertEqual("see src/adapters/tool/b.cpp and src/adapters/tool/c.cpp; tests in tests/agent/\n", self.read("docs/guide.md"))
        self.assertEqual('#include "utils/a.hpp"\n#include "c.hpp"\n', self.read("src/adapters/tool/c.cpp"))
        m2 = commit(self.root, "P3", "M2", "[mechanical]")

        blame = step_blame(self.root, [m1, m2], "P3 M1/M2", dry_run=False)
        self.assertEqual([m1, m2], blame["revs"])
        self.assertEqual([], blame["warnings"])
        text = self.read(".git-blame-ignore-revs")
        self.assertIn("# P3 M1/M2\n" + m1 + "\n" + m2 + "\n", text)
        again = step_blame(self.root, [m1], "P3 M1/M2", dry_run=False)
        self.assertEqual([], again["revs"])  # 重复登记只告警不追加

    def test_rewrite_keeps_index_eol_when_checkout_uses_autocrlf(self):
        # 触发场景:检出配置 core.autocrlf=true(Windows 常见),工作区是 CRLF、索引里的 blob 是 LF。
        # 期望:rewrite 后提交的 blob 仍是 LF,只有真正改动的行进入 diff。回归表现:用 migration_git 的
        # 固定 autocrlf=false 去 add,会把整份 CRLF 写进 blob,M2 的「机械改写」变成整文件重写。
        # 夹具用 migration_git 的 git(固定 autocrlf=false)建库;这里改用工具自己的 git 包装按用户配置重新检出成 CRLF。
        user_git(self.root, "config", "core.autocrlf", "true")
        user_git(self.root, "rm", "-rq", "--cached", ".")
        user_git(self.root, "reset", "-q", "--hard", "HEAD")
        self.assertIn(b"\r\n", (self.root / "docs/guide.md").read_bytes())
        step_move(self.root, self.rows, ["P2-08"], dry_run=False, allow_unfinished=False, allow_dirty=False)
        commit(self.root, "P2-08", "搬迁", "[mechanical]")
        step_rewrite(self.root, self.map, self.rows, ["P2-08"], dry_run=False, design_note=False)
        commit(self.root, "P2-08", "改写", "[mechanical]")
        blob = git(self.root, "show", "HEAD:docs/guide.md")
        self.assertNotIn(b"\r", blob)
        self.assertIn(b"see src/tool/b.cpp", blob)
        numstat = git(self.root, "diff", "--numstat", "HEAD~1", "HEAD", "--", "docs/guide.md").decode().split()
        self.assertEqual(["1", "1"], numstat[:2])

    def test_move_refuses_dirty_tree_and_existing_destination(self):
        # 触发场景:工作区有未提交改动 / 目标位置已有未跟踪文件。期望:都在写之前拒绝,索引保持不动。
        self.write("src/utils/a.cpp", '#include "utils/a.hpp"\nint x;\n')
        with self.assertRaises(ValueError):
            step_move(self.root, self.rows, ["P2-08"], dry_run=False, allow_unfinished=False, allow_dirty=False)
        git(self.root, "checkout", "--", "src/utils/a.cpp")
        self.write("src/tool/b.cpp", "untracked\n")
        with self.assertRaises(ValueError):
            step_move(self.root, self.rows, ["P2-08"], dry_run=False, allow_unfinished=False, allow_dirty=False)
        self.assertEqual([], self.staged())
        self.assertEqual("base", git(self.root, "log", "-1", "--format=%s", revision(self.root, "HEAD")).decode().strip())


if __name__ == "__main__":
    unittest.main()
