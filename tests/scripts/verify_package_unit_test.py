#!/usr/bin/env python3
"""Focused cross-platform tests for the local package verifier."""

from __future__ import annotations

import hashlib
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = (
    REPO_ROOT / ".acecode" / "skills" / "verify-package" /
    "scripts" / "verify_package.py"
)


def load_verify_package_module():
    spec = importlib.util.spec_from_file_location("verify_package_under_test", SCRIPT_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


verify_package = load_verify_package_module()


class FakeProcess:
    def __init__(self, first_wait_times_out: bool, kill_raises: bool = False) -> None:
        self.first_wait_times_out = first_wait_times_out
        self.kill_raises = kill_raises
        self.terminate_calls = 0
        self.kill_calls = 0
        self.wait_calls = 0

    def terminate(self) -> None:
        self.terminate_calls += 1

    def kill(self) -> None:
        self.kill_calls += 1
        if self.kill_raises:
            raise ProcessLookupError("fake process already exited")

    def wait(self, timeout: int):
        self.wait_calls += 1
        if self.first_wait_times_out and self.wait_calls == 1:
            raise subprocess.TimeoutExpired("fake-desktop", timeout)
        return -9 if self.kill_calls else 0


class VerifyPackageUnitTest(unittest.TestCase):
    def test_windows_does_not_force_ninja_and_maps_cmake_targets(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            root = Path(root_text)
            repo = root / "repo"
            build = root / "build"
            repo.mkdir()
            commands = []

            def capture(_report, _name, command, **_kwargs):
                commands.append(command)
                return True

            with mock.patch.object(verify_package, "run_tool", side_effect=capture), \
                    mock.patch.object(verify_package.shutil, "which", return_value="ninja"):
                result = verify_package.configure_and_build(
                    verify_package.Report(), repo, build, "cmake",
                    ["tui", "desktop"], "windows"
                )

            self.assertTrue(result)
            self.assertNotIn("-G", commands[0])
            self.assertEqual(commands[1][-1], "acecode")
            self.assertEqual(commands[2][-1], "acecode-desktop")

    def test_non_windows_prefers_ninja(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            root = Path(root_text)
            repo = root / "repo"
            build = root / "build"
            repo.mkdir()
            commands = []

            def capture(_report, _name, command, **_kwargs):
                commands.append(command)
                return True

            with mock.patch.object(verify_package, "run_tool", side_effect=capture), \
                    mock.patch.object(verify_package.shutil, "which", return_value="ninja"):
                verify_package.configure_and_build(
                    verify_package.Report(), repo, build, "cmake", ["tui"], "linux"
                )

            self.assertEqual(commands[0][4:6], ["-G", "Ninja"])

    def test_staging_path_guard_rejects_protected_paths(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            root = Path(root_text).resolve()
            repo = root / "repo"
            build = repo / "build"
            cwd = root / "cwd"
            home = root / "home"
            build.mkdir(parents=True)
            cwd.mkdir()
            home.mkdir()

            self.assertIsNotNone(verify_package.validate_staging_path(
                repo, build, repo, cwd=cwd, home=home))
            self.assertIsNotNone(verify_package.validate_staging_path(
                repo, build, build, cwd=cwd, home=home))
            self.assertIsNotNone(verify_package.validate_staging_path(
                repo, build, repo / "unrelated", cwd=cwd, home=home))
            self.assertIsNotNone(verify_package.validate_staging_path(
                repo, build, cwd, cwd=cwd, home=home))
            self.assertIsNone(verify_package.validate_staging_path(
                repo, build, build / "verify-package-staging", cwd=cwd, home=home))
            self.assertIsNone(verify_package.validate_staging_path(
                repo, build, root / "outside-staging", cwd=cwd, home=home))

    def test_force_kill_path_is_waited_and_reaped(self) -> None:
        process = FakeProcess(first_wait_times_out=True)
        self.assertTrue(verify_package.terminate_and_reap(process, timeout=1))
        self.assertEqual(process.terminate_calls, 1)
        self.assertEqual(process.kill_calls, 1)
        self.assertEqual(process.wait_calls, 2)

    def test_kill_race_still_waits_to_reap(self) -> None:
        process = FakeProcess(first_wait_times_out=True, kill_raises=True)
        self.assertTrue(verify_package.terminate_and_reap(process, timeout=1))
        self.assertEqual(process.kill_calls, 1)
        self.assertEqual(process.wait_calls, 2)

    def test_skill_script_copies_are_identical(self) -> None:
        copies = [
            REPO_ROOT / root / "skills" / "verify-package" /
            "scripts" / "verify_package.py"
            for root in (".acecode", ".agents", ".claude", ".codex")
        ]
        hashes = {
            hashlib.sha256(path.read_bytes()).hexdigest()
            for path in copies
        }
        self.assertEqual(len(hashes), 1, copies)


if __name__ == "__main__":
    unittest.main()
