"""Exercise the production append and trace writers in independent processes."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

FIXTURE = Path(sys.argv.pop(1)).resolve()
CREATE_FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


class RuntimeLogAppendTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="acecode-runtime-logs-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def invoke(self, *args):
        return subprocess.run([str(FIXTURE), *map(str, args)], cwd=self.root,
                              capture_output=True, text=True, timeout=30,
                              creationflags=CREATE_FLAGS, check=True)

    def assert_records(self, text, processes, count):
        lines = text.splitlines()
        records = [re.search(r"record=(\d+):(\d+) (x{300})$", line) for line in lines]
        self.assertEqual(len(lines), processes * count)
        self.assertTrue(all(records), "truncated or interleaved log record")
        self.assertEqual({(int(m[1]), int(m[2])) for m in records},
                         {(p, i) for p in range(processes) for i in range(count)})

    def test_missing_unicode_directory_and_separate_trace(self):
        target = self.root / "中文数据" / "logs"
        self.invoke("logger", target, 0, 1)
        self.invoke("trace", target, 0, 1)
        self.assertTrue(target.is_dir())
        primary = list(target.glob("tui_????_??_??.log"))
        trace = list(target.glob("tui_input_trace_????_??_??.log"))
        self.assertEqual(len(primary), 1)
        self.assertEqual(len(trace), 1)
        self.assert_records(primary[0].read_text(), 1, 1)
        self.assert_records(trace[0].read_text(), 1, 1)
        self.assertFalse((self.root / "acecode.log").exists())

    def test_processes_preserve_every_complete_record(self):
        for mode in ("raw", "logger", "trace"):
            with self.subTest(mode=mode):
                target = self.root / mode
                processes, count = 4, 2000
                children = [subprocess.Popen(
                    [str(FIXTURE), mode, str(target), str(p), str(count)],
                    cwd=self.root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    creationflags=CREATE_FLAGS) for p in range(processes)]
                try:
                    for child in children:
                        self.assertEqual(child.wait(timeout=30), 0)
                finally:
                    for child in children:
                        if child.poll() is None:
                            child.kill()
                            child.wait(timeout=5)
                paths = [target] if mode == "raw" else list(target.glob("*.log"))
                self.assertEqual(len(paths), 1)
                self.assert_records(paths[0].read_text(), processes, count)

    def test_trace_uses_local_date_across_midnight(self):
        result = self.invoke("trace-path", self.root)
        self.assertEqual(result.stdout.splitlines(), [
            "tui_input_trace_2026_09_20.log", "tui_input_trace_2026_09_21.log"])

    def test_trace_refuses_unconfigured_relative_and_unopenable_paths(self):
        blocked = self.root / "regular-file"
        blocked.write_text("preserved")
        for target in ("", "relative", blocked / "logs"):
            with self.subTest(target=target):
                self.invoke("trace-reject", target)
        self.assertEqual(blocked.read_text(), "preserved")
        self.assertFalse((self.root / "relative").exists())
        self.assertFalse((self.root / "acecode.log").exists())


if __name__ == "__main__":
    unittest.main()
