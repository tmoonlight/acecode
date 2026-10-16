"""Production-path regressions found during development launcher review."""

import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import dev_environment as launcher


class DevelopmentLauncherReviewTest(unittest.TestCase):
    def make_candidate(self, root, configuration=None):
        root = root.resolve()
        build = root / "build" / "configured"
        build.mkdir(parents=True)
        cache = "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(root.resolve()) + "\nACECODE_BUILD_DESKTOP:BOOL=ON\n"
        if configuration:
            cache += "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n"
        (build / "CMakeCache.txt").write_text(cache, encoding="utf-8")
        output = build / configuration if configuration else build
        output.mkdir(exist_ok=True)
        executable = output / launcher.native_executable_name("acecode-desktop")
        executable.touch()
        executable.chmod(0o755)
        return launcher.BuildCandidate(build, root, executable, configuration)

    def test_multiconfig_build_and_desktop_launch_use_same_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = self.make_candidate(root, "Debug")
            release = expected.build_dir / "Release" / expected.executable.name
            release.parent.mkdir()
            release.touch()
            release.chmod(0o755)
            candidate = launcher.find_compatible_build(root, "desktop")
            self.assertEqual(candidate.configuration, "Debug")
            with patch.object(launcher.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
                self.assertTrue(launcher.build_target(root, candidate.build_dir, "desktop", None, candidate.configuration))
                self.assertEqual(run.call_args.args[0][-2:], ["--config", "Debug"])
                self.assertEqual(launcher.launch_surface(root, "desktop", candidate, False, []), 0)
                command = run.call_args.args[0]
            self.assertEqual(command[command.index("--build-dir") + 1], str(expected.executable))
            self.assertNotIn(str(release), command)

    def test_reconfigure_preserves_existing_build_type_and_testing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = self.make_candidate(root)
            with patch.object(launcher.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
                self.assertTrue(launcher.configure_build(root, "ignored", candidate.build_dir, None))
            command = run.call_args.args[0]
            self.assertFalse(any(arg.startswith("-DCMAKE_BUILD_TYPE=") or arg.startswith("-DBUILD_TESTING=") for arg in command))

    def test_relative_build_directory_is_resolved_against_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = self.make_candidate(root)
            actual = launcher.find_compatible_build(root, "desktop", Path("build/configured"))
            self.assertEqual(actual.build_dir, expected.build_dir)

    def test_nested_preset_artifact_is_not_attributed_to_parent_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            expected = self.make_candidate(root)
            parent = root / "build"
            (parent / "CMakeCache.txt").write_text(
                "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(root) + "\nACECODE_BUILD_DESKTOP:BOOL=ON\n", encoding="utf-8")
            self.assertIsNone(launcher.executable_for(parent, "desktop"))
            self.assertEqual(launcher.find_compatible_build(root, "desktop").build_dir, expected.build_dir)

    def test_windows_tui_uses_no_shell_and_preserves_arguments(self):
        root = Path("C:/dev/ACE & test%PATH%!")
        executable = root / "acecode.exe"
        candidate = launcher.BuildCandidate(root, root, executable)
        extra = ["--cwd", "C:/project with spaces", "--prompt", 'quote " & %PATH% ! $x 中文']
        with patch.object(launcher.os, "name", "nt"), \
             patch.object(launcher.subprocess, "CREATE_NEW_CONSOLE", 0x10, create=True), \
             patch.object(launcher.subprocess, "Popen") as popen:
            self.assertEqual(launcher.launch_surface(root, "tui", candidate, False, extra), 0)
        self.assertEqual(popen.call_args.args[0], [str(executable), *extra])
        self.assertEqual(popen.call_args.kwargs, {"cwd": root, "creationflags": 0x10})

    def test_macos_tui_preserves_cwd_and_shell_characters(self):
        root = Path("/tmp/ACE ' & workspace")
        executable = root / "acecode"
        extra = ["--prompt", 'literal `echo wrong` $(echo wrong) "quotes" 中文']
        with patch.object(launcher.os, "name", "posix"), patch.object(launcher.sys, "platform", "darwin"):
            command = launcher.tui_command(root, executable, extra)
        self.assertEqual(command[:2], ["osascript", "-e"])
        quoted_command = command[2].split("do script ", 1)[1].split("\nend tell", 1)[0]
        self.assertEqual(shlex.split(json.loads(quoted_command)), ["cd", str(root), "&&", "exec", str(executable), *extra])

    def test_linux_tui_forwards_each_argument(self):
        root = Path("/tmp/ACE & workspace")
        for terminal, separator in (("gnome-terminal", "--"), ("konsole", "-e"), ("xterm", "-e")):
            with self.subTest(terminal=terminal), patch.object(launcher.os, "name", "posix"), \
                 patch.object(launcher.sys, "platform", "linux"), \
                 patch.object(launcher.shutil, "which", side_effect=lambda name: f"/usr/bin/{name}" if name == terminal else None):
                self.assertEqual(launcher.tui_command(root, root / "acecode", ["--prompt", "two words"]),
                                 [f"/usr/bin/{terminal}", separator, str(root / "acecode"), "--prompt", "two words"])

    def test_surface_script_failure_is_returned(self):
        root = Path("C:/work")
        candidate = launcher.BuildCandidate(root, root, root / "acecode-desktop.exe")
        with patch.object(launcher.subprocess, "run", return_value=subprocess.CompletedProcess([], 7)):
            self.assertEqual(launcher.launch_surface(root, "desktop", candidate, False, ["--invalid"]), 7)

    def test_mingw_is_compatible_and_does_not_require_visual_studio(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = self.make_candidate(root)
            cache = candidate.build_dir / "CMakeCache.txt"
            cache.write_text(cache.read_text(encoding="utf-8") + "VCPKG_TARGET_TRIPLET:STRING=x64-mingw-static\n", encoding="utf-8")
            with patch.object(launcher.platform, "system", return_value="Windows"), \
                 patch.object(launcher, "native_machine", return_value="amd64"), \
                 patch.object(launcher.subprocess, "run") as run:
                self.assertTrue(launcher.platform_matches(candidate.build_dir))
                self.assertFalse(launcher.needs_msvc(candidate))
                self.assertTrue(launcher.ensure_windows_environment(root, candidate))
            run.assert_not_called()

    def test_every_default_preset_exists(self):
        presets = {item["name"] for item in json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))["configurePresets"]}
        for system in ("Windows", "Darwin", "Linux"):
            for machine in ("amd64", "arm64"):
                for target in launcher.LAUNCH_TARGETS:
                    with self.subTest(system=system, machine=machine, target=target), \
                         patch.object(launcher.platform, "system", return_value=system), \
                         patch.object(launcher, "native_machine", return_value=machine):
                        self.assertIn(launcher.default_preset(target), presets)

    def test_arm64_windows_emulation_selects_native_architecture(self):
        with patch.object(launcher.platform, "system", return_value="Windows"), \
             patch.object(launcher.platform, "machine", return_value="AMD64"), \
             patch.dict(os.environ, {"PROCESSOR_ARCHITECTURE": "AMD64", "PROCESSOR_ARCHITEW6432": "ARM64"}):
            self.assertEqual(launcher.default_preset("desktop"), "windows-arm64-desktop-release")

    @unittest.skipUnless(os.name == "nt", "Windows environment setup")
    def test_windows_environment_helper_preserves_metacharacters_in_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "ACE & %PATH% ! repo"
            (root / "scripts").mkdir(parents=True)
            (root / "scripts/dev_windows_env.bat").write_text(
                "@echo off\nset ACECODE_TEST_ENV=ready\nset\n", encoding="ascii")
            with patch.dict(os.environ, {}, clear=False):
                self.assertTrue(launcher.ensure_windows_environment(root, None))
                self.assertEqual(os.environ.get("ACECODE_TEST_ENV"), "ready")

    @unittest.skipUnless(os.name == "nt", "Windows environment setup")
    def test_missing_msvc_environment_fails_before_build(self):
        root = Path("C:/missing-tools")
        with patch.object(launcher.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, stdout="[ERROR] Missing C++ tools", stderr="")), \
             contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertFalse(launcher.ensure_windows_environment(root, None))
        self.assertIn("Missing C++ tools", output.getvalue())

    def test_existing_runtime_only_runs_identity_check_never_stop(self):
        for status in (0, 1):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                candidate = self.make_candidate(root)
                run_dir = root / "explicit-runtime"
                run_dir.mkdir()
                (run_dir / "daemon.pid").write_text("123", encoding="ascii")
                error_output = io.StringIO()
                with patch.object(launcher.subprocess, "run", return_value=subprocess.CompletedProcess([], status)) as run, \
                     contextlib.redirect_stderr(error_output):
                    self.assertFalse(launcher.web_runtime_is_available(root, candidate, [f"--run-dir={run_dir}"]))
                self.assertEqual(run.call_args.args[0][1:3], ["daemon", "status"])
                self.assertEqual(run.call_args.kwargs["timeout"], 10)
                self.assertEqual((run_dir / "daemon.pid").read_text(encoding="ascii"), "123")
                self.assertIn("Stop this verified daemon" if status == 0 else "Runtime identity is unverified", error_output.getvalue())

    def test_runtime_failure_prevents_configuration_build_and_launch(self):
        root = Path("C:/work")
        candidate = launcher.BuildCandidate(root, root, root / "acecode.exe")
        args = argparse.Namespace(target="web", build_dir=None, yes=True, dry_run=False, extra=[])
        with patch.object(launcher, "parse_args", return_value=args), \
             patch.object(launcher, "project_root", return_value=root), \
             patch.object(launcher, "find_sccache", return_value=None), \
             patch.object(launcher, "find_compatible_build", return_value=candidate), \
             patch.object(launcher, "web_runtime_is_available", return_value=False), \
             patch.object(launcher, "ensure_windows_environment") as environment, \
             patch.object(launcher, "configure_build") as configure, \
             patch.object(launcher, "build_target") as build, \
             patch.object(launcher, "launch_surface") as launch:
            self.assertEqual(launcher.main(), 1)
        environment.assert_not_called()
        configure.assert_not_called()
        build.assert_not_called()
        launch.assert_not_called()

    def test_runtime_from_previous_commit_also_blocks_rebuild(self):
        for base in ("dev-run", "dev_run"):
            for identity in ("work-tree-oldcommit", "work_tree_oldcommit"):
                with self.subTest(base=base, identity=identity), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve() / "work-tree"
                    root.mkdir()
                    candidate = self.make_candidate(root)
                    # Both generations can coexist after an upgrade; an empty legacy
                    # root must not hide a worker using the underscore naming scheme.
                    for name in ("dev-run", "dev_run"):
                        (root / ".acecode" / name).mkdir(parents=True)
                    previous = root / ".acecode" / base / identity
                    previous.mkdir()
                    (previous / "daemon.pid").write_text("123", encoding="ascii")
                    with patch.object(launcher, "current_commit", return_value="newcommit"), \
                         patch.object(launcher.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run, \
                         contextlib.redirect_stderr(io.StringIO()):
                        self.assertFalse(launcher.web_runtime_is_available(root, candidate, []))
                    self.assertEqual(run.call_args.args[0][-1], f"--run-dir={previous}")
                    self.assertEqual(run.call_args.args[0][1:3], ["daemon", "status"])
                    self.assertEqual((previous / "daemon.pid").read_text(encoding="ascii"), "123")

    def test_explicit_runtime_does_not_inspect_other_launcher_daemons(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            candidate = self.make_candidate(root)
            previous = root / ".acecode/dev-run" / (root.name + "-oldcommit")
            previous.mkdir(parents=True)
            (previous / "daemon.pid").write_text("123", encoding="ascii")
            with patch.object(launcher.subprocess, "run") as run:
                self.assertTrue(launcher.web_runtime_is_available(root, candidate, ["--run-dir", "custom-runtime"]))
            run.assert_not_called()

    def test_desktop_list_does_not_build_or_initialize_toolchain(self):
        args = argparse.Namespace(target="desktop", build_dir=Path("build/custom"), yes=True, dry_run=False, extra=["--list"])
        with patch.object(launcher, "parse_args", return_value=args), \
             patch.object(launcher, "find_sccache") as discover, \
             patch.object(launcher.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            self.assertEqual(launcher.main(), 0)
        discover.assert_not_called()
        self.assertIn("--list", run.call_args.args[0])

    def test_force_web_rebuild_does_not_reuse_existing_assets(self):
        builder = Mock()
        builder.ensure_node_and_pnpm.return_value = ("node", "pnpm")
        root = Path("C:/work")
        with patch.object(launcher, "load_web_builder", return_value=builder), \
             patch.object(launcher, "seed_web_assets") as seed:
            self.assertTrue(launcher.refresh_web_assets(root, force=True))
        seed.assert_not_called()
        builder.build_web.assert_called_once_with(root / "web", "pnpm", force=True)

    def test_desktop_list_dry_run_does_not_spawn_a_process(self):
        args = argparse.Namespace(target="desktop", build_dir=None, yes=False, dry_run=True, extra=["--list"])
        with patch.object(launcher, "parse_args", return_value=args), \
             patch.object(launcher.subprocess, "run") as run:
            self.assertEqual(launcher.main(), 0)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
