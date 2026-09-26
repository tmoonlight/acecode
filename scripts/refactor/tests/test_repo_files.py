import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from repo_files import byte_lines, git, replace_path_prefix, tracked_files
from branch_inventory import inventory


class RepositoryToolsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        git(self.root, "init", "-b", "master")
        git(self.root, "config", "user.email", "tests@example.invalid")
        git(self.root, "config", "user.name", "Refactor tests")

    def write(self, path, content):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)

    def commit(self, message):
        git(self.root, "add", ".")
        git(self.root, "commit", "-m", message)

    def test_tracked_files_do_not_enter_nested_worktrees(self):
        # 即使嵌套 worktree 的路径误被跟踪，也不能把其中的代码纳入重写。
        self.write("src/中文.hpp", b"one\n")
        self.write(".claude/worktrees/old/src/x.hpp", b"old\n")
        self.write(".worktrees/old/src/x.hpp", b"old\n")
        self.write(".acecode/worktrees/old/src/x.hpp", b"old\n")
        self.commit("fixture")
        self.write("src/untracked.hpp", b"ignored\n")
        self.assertEqual(["src/中文.hpp"], tracked_files(self.root))

    def test_prefix_replacement_preserves_mixed_line_endings(self):
        # 混排 CRLF/LF 和末尾无换行的文件必须逐字节保持非路径内容。
        before = b'"src/tool/x.hpp"\r\n"src/tool_preamble/y.hpp"\nweb/src/tool/x\r\n"src/tool/z.hpp"'
        after = replace_path_prefix(before, "src/tool/", "src/adapters/tool/")
        self.assertEqual(after, b'"src/adapters/tool/x.hpp"\r\n"src/tool_preamble/y.hpp"\nweb/src/tool/x\r\n"src/adapters/tool/z.hpp"')
        self.assertEqual([b"a\r\n", b"b\n", b"c"], byte_lines(b"a\r\nb\nc"))
        with self.assertRaises(ValueError):
            replace_path_prefix(before, "src/tool", "src/adapters/tool/")

    def test_inventory_distinguishes_unique_and_equivalent_patches(self):
        # 两个 SHA 不同但补丁相等的提交必须标为等价，不能误报独有 src 改动。
        self.write("src/base.hpp", b"base\n")
        self.commit("base")
        git(self.root, "switch", "-c", "feature")
        self.write("src/feature.hpp", b"feature\n")
        self.commit("feature")
        feature = git(self.root, "rev-parse", "HEAD").decode().strip()
        git(self.root, "switch", "master")
        self.write("README.md", b"different parent\n")
        self.commit("main work")
        git(self.root, "cherry-pick", feature)
        git(self.root, "switch", "-c", "unique")
        self.write("tests/new_test.cpp", b"new\n")
        self.commit("unique")
        self.write("dirty.txt", b"dirty\n")
        report = inventory(self.root, "master")
        by_ref = {b["ref"]: b for b in report["branches"]}
        self.assertEqual(1, by_ref["refs/heads/feature"]["equivalent_commits"])
        self.assertEqual([], by_ref["refs/heads/feature"]["src_paths"])
        self.assertEqual(["tests/new_test.cpp"], by_ref["refs/heads/unique"]["test_paths"])
        self.assertTrue(report["worktrees"][0]["dirty"])


if __name__ == "__main__":
    unittest.main()
