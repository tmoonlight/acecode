#!/usr/bin/env python3
"""Focused cross-platform tests for the local package verifier."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import subprocess
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
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
    @unittest.skipUnless(shutil.which('cmake'), 'CMake required for generator integration')
    def test_parallel_build_with_host_default_generator(self) -> None:
        # A language-free project exercises real MSBuild on Windows without
        # needing ACECode's vcpkg dependencies; Unix hosts use their default.
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / 'source'
            repo.mkdir()
            build = Path(directory) / 'build'
            (repo / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(ParallelBuild NONE)\n'
                'add_custom_target(acecode COMMAND ${CMAKE_COMMAND} -E touch '
                '${CMAKE_BINARY_DIR}/built.txt)\n', encoding='utf-8')
            configured = subprocess.run(
                ['cmake', '-S', str(repo), '-B', str(build)], capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            self.assertTrue(verify_package.configure_and_build(
                verify_package.Report(), repo, build, 'cmake', None, ['tui'],
                'windows' if sys.platform == 'win32' else 'linux', jobs=2))
            self.assertTrue((build / 'built.txt').is_file())

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
                    verify_package.Report(), repo, build, "cmake", None,
                    ["tui", "desktop"], "windows", jobs=4
                )

            self.assertTrue(result)
            self.assertNotIn("-G", commands[0])
            self.assertEqual(
                commands[1][commands[1].index("--target") + 1], "acecode")
            self.assertEqual(
                commands[2][commands[2].index("--target") + 1], "acecode-desktop")
            for command in commands[1:]:
                self.assertEqual(command[-2:], ["--parallel", "4"])

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
                    verify_package.Report(), repo, build, "cmake", "ninja", ["tui"], "linux",
                    jobs=4
                )

            self.assertEqual(commands[0][1:3], ["-G", "Ninja"])
            self.assertEqual(commands[1][-2:], ["--parallel", "4"])

    def test_windows_stages_computer_use_component_for_cli_and_desktop(self) -> None:
        for targets in (["tui"], ["desktop"]):
            with self.subTest(targets=targets), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                repo, build, staging = root / "repo", root / "build", root / "staging"
                repo.mkdir()
                build.mkdir()
                for name in ("acecode.exe", "acecode-desktop.exe"):
                    (build / name).write_bytes(b"fixture")
                commands = []

                def capture(_report, _name, command, **_kwargs):
                    commands.append(command)
                    return True

                with mock.patch.object(verify_package, "run_tool", side_effect=capture):
                    self.assertTrue(verify_package.stage(
                        verify_package.Report(), repo, build, staging,
                        "windows", targets, "cmake"))
                components = [command[command.index("--component") + 1] for command in commands]
                self.assertIn("computer_use_runtime", components)

    def test_windows_missing_or_empty_computer_use_runtime_fails_structure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            helper = root / "acecode-computer-use.exe"
            with mock.patch.object(verify_package, "check_models_dev"), \
                    mock.patch.object(verify_package, "check_seed_bundle"):
                for state in ("missing", "empty", "present"):
                    with self.subTest(state=state):
                        if state != "missing":
                            helper.write_bytes(b"runtime" if state == "present" else b"")
                        report = verify_package.Report()
                        verify_package.structural_checks(report, root, root, "windows", ["tui"])
                        self.assertEqual(report.failed, 0 if state == "present" else 1)

    def test_computer_use_install_is_windows_and_macos_only_in_dry_run(self) -> None:
        for platform in ("windows", "darwin", "linux"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                output = io.StringIO()
                with redirect_stdout(output):
                    verify_package.print_dry_run(root, root / "build", root / "staging",
                                                platform, ["tui"], 2, "cmake", None, True)
                self.assertEqual("--component computer_use_runtime" in output.getvalue(),
                                 platform in ("windows", "darwin"))

    def test_macos_computer_use_helper_must_be_executable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            helper = root / "acecode-computer-use"
            helper.write_bytes(b"runtime")
            with mock.patch.object(verify_package, "check_models_dev"), \
                    mock.patch.object(verify_package, "check_seed_bundle"):
                for mode, failures in ((0o644, 1), (0o755, 0)):
                    helper.chmod(mode)
                    report = verify_package.Report()
                    with mock.patch.object(verify_package.os, "access",
                                           wraps=verify_package.os.access) as access:
                        # Windows chmod has no POSIX execute bits. Simulate that
                        # boundary here while retaining real mode checks on Unix.
                        if sys.platform == "win32":
                            access.return_value = bool(mode & 0o111)
                        verify_package.structural_checks(report, root, root, "darwin", ["tui"])
                        access.assert_called_once_with(helper, verify_package.os.X_OK)
                    self.assertEqual(report.failed, failures)

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
