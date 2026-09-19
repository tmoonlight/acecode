import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("dev_web", ROOT / "scripts/dev_web.py")
dev_web = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dev_web)


class DevWebTest(unittest.TestCase):
    def test_finds_platform_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            name = "acecode.exe" if os.name == "nt" else "acecode"
            executable = root / "Release" / name
            executable.parent.mkdir()
            executable.touch()
            self.assertEqual(dev_web.find_executable(root), executable)
            direct = root / name
            direct.touch()
            self.assertEqual(dev_web.find_executable(root), direct)

    def run_launcher(self, exit_code, port):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "web/dist").mkdir(parents=True)
            (root / "web/dist/index.html").write_text("test", encoding="utf-8")
            with patch.object(dev_web, "find_project_root", return_value=root), \
                 patch.object(dev_web, "find_executable", return_value=root / "acecode.exe"), \
                 patch.object(sys, "argv", ["dev_web.py", "--no-browser"]), \
                 patch.object(dev_web.subprocess, "run", return_value=subprocess.CompletedProcess([], exit_code)), \
                 patch.object(dev_web, "_wait_for_port", return_value=port) as wait, \
                 patch.object(dev_web, "_open_web_ui") as open_ui:
                result = dev_web.main()
                return result, wait.call_count, open_ui.call_args

    def test_worker_timeout_opens_a_fresh_port(self):
        result, reads, opened = self.run_launcher(3, 12345)
        self.assertEqual(result, 0)
        self.assertEqual(reads, 1)
        self.assertEqual(opened.kwargs, {"already_running": False})

    def test_verified_running_daemon_can_be_opened(self):
        result, reads, opened = self.run_launcher(6, 12345)
        self.assertEqual(result, 0)
        self.assertEqual(reads, 1)
        self.assertEqual(opened.kwargs, {"already_running": True})

    def test_successful_launch_without_port_fails(self):
        result, _, opened = self.run_launcher(0, None)
        self.assertEqual(result, 1)
        self.assertIsNone(opened)

    @unittest.skipUnless(os.name == "nt", "Windows batch wrapper")
    def test_batch_falls_back_to_py_and_preserves_exit_code(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "py.bat").write_text("@echo off\necho fake-python-launcher\nexit /b 7\n", encoding="ascii")
            environment = dict(os.environ)
            environment["PATH"] = str(root) + os.pathsep + str(Path(os.environ["SystemRoot"]) / "System32")
            result = subprocess.run(
                [os.environ["COMSPEC"], "/d", "/c", str(ROOT / "scripts/dev_web.bat"), "--help"],
                cwd=root, env=environment, capture_output=True, text=True,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
            self.assertIn("fake-python-launcher", result.stdout)


if __name__ == "__main__":
    unittest.main()
