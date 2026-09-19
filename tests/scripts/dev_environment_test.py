import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import subprocess
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("dev_environment", ROOT / "scripts/dev_environment.py")
dev_environment = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = dev_environment
spec.loader.exec_module(dev_environment)


class DevEnvironmentTest(unittest.TestCase):
    def make_build(self, root, desktop=False, name="test"):
        build = root / "build" / name
        build.mkdir(parents=True)
        (build / "CMakeCache.txt").write_text(
            "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(root) + "\n"
            "VCPKG_TARGET_TRIPLET:STRING=x64-windows-static\n"
            f"ACECODE_BUILD_DESKTOP:BOOL={'ON' if desktop else 'OFF'}\n",
            encoding="utf-8",
        )
        (build / dev_environment.native_executable_name("acecode")).touch()
        if desktop:
            (build / dev_environment.native_executable_name("acecode-desktop")).touch()
        return build

    def test_web_candidate_prefers_non_desktop_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            plain = self.make_build(root, name="plain")
            self.make_build(root, desktop=True, name="desktop")
            with patch.object(dev_environment, "current_commit", return_value="same"), \
                 patch.object(dev_environment, "registered_worktrees", return_value=[root]), \
                 patch.object(dev_environment, "platform_matches", return_value=True):
                candidate = dev_environment.find_compatible_build(root, "web")
            self.assertEqual(candidate.build_dir, plain.resolve())

    def test_candidate_does_not_reuse_another_worktree_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "current"
            other = Path(directory) / "other"
            root.mkdir()
            other.mkdir()
            self.make_build(other)
            with patch.object(dev_environment, "registered_worktrees", return_value=[root, other]), \
                 patch.object(dev_environment, "platform_matches", return_value=True):
                self.assertIsNone(dev_environment.find_compatible_build(root, "web"))

    def test_registered_worktrees_parses_porcelain_prefix(self):
        root = Path("C:/work")
        output = "worktree C:/work\nHEAD abc\n\nworktree C:/other\nHEAD def\n"
        completed = subprocess.CompletedProcess([], 0, stdout=output)
        with patch.object(dev_environment.subprocess, "run", return_value=completed):
            self.assertEqual(dev_environment.registered_worktrees(root), [Path("C:/work"), Path("C:/other")])

    def test_desktop_candidate_requires_desktop_configuration_and_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = self.make_build(root, desktop=False)
            with patch.object(dev_environment, "current_commit", return_value="same"), \
                 patch.object(dev_environment, "registered_worktrees", return_value=[root]), \
                 patch.object(dev_environment, "platform_matches", return_value=True):
                self.assertIsNone(dev_environment.find_compatible_build(root, "desktop"))
            self.assertTrue((build / dev_environment.native_executable_name("acecode")).exists())

    def test_tui_command_uses_new_windows_terminal(self):
        executable = Path("C:/work/build/acecode.exe")
        with patch.object(dev_environment.os, "name", "nt"):
            command = dev_environment.tui_command(Path("C:/work"), executable)
        self.assertEqual(command[:4], ["cmd.exe", "/d", "/c", "start"])
        self.assertIn(str(executable), command)

    def test_runtime_directory_is_scoped_to_worktree(self):
        root = Path("C:/worktrees/my project")
        with patch.object(dev_environment, "current_commit", return_value="abcdef1234567890"):
            runtime = dev_environment.worktree_runtime_dir(root)
        self.assertEqual(runtime, root / ".acecode/dev-run/my-project-abcdef123456")

    def test_noninteractive_target_selection_fails(self):
        with patch.object(dev_environment.sys.stdin, "isatty", return_value=False):
            self.assertIsNone(dev_environment.choose_target(None))

    def test_sccache_discovery_prefers_usable_path_and_has_install_hint(self):
        completed = subprocess.CompletedProcess([], 0, stdout="sccache 1.0")
        with patch.object(dev_environment.shutil, "which", return_value="C:/tools/sccache.exe"), \
             patch.object(Path, "is_file", return_value=True), \
             patch.object(dev_environment.subprocess, "run", return_value=completed):
            self.assertEqual(dev_environment.find_sccache(), Path("C:/tools/sccache.exe"))
        self.assertIn("sccache", dev_environment.sccache_install_hint())

    def test_sccache_discovery_rejects_unusable_file(self):
        completed = subprocess.CompletedProcess([], 1, stdout="")
        with patch.object(dev_environment, "sccache_candidates", return_value=[Path("C:/broken/sccache.exe")]), \
             patch.object(Path, "is_file", return_value=True), \
             patch.object(dev_environment.subprocess, "run", return_value=completed):
            self.assertIsNone(dev_environment.find_sccache())

    def test_cache_state_detects_configuration_transition(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            cache = Path("C:/tools/sccache.exe")
            self.assertTrue(dev_environment.cache_state_changed(build, cache))
            dev_environment.write_sccache_marker(build, cache)
            self.assertFalse(dev_environment.cache_state_changed(build, cache))
            self.assertTrue(dev_environment.cache_state_changed(build, None))

    def test_sccache_build_failure_disables_only_matching_cache_path(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory) / "build"
            cache = Path("C:/tools/sccache.exe")
            dev_environment.write_sccache_marker(build, cache, enabled=False)
            self.assertTrue(dev_environment.sccache_disabled_for_build(build, cache))
            self.assertFalse(dev_environment.sccache_disabled_for_build(build, Path("C:/tools/sccache-new.exe")))

    def test_configure_and_build_disables_test_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(dev_environment.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run, \
                 patch.object(dev_environment, "build_target", return_value=True):
                self.assertEqual(
                    dev_environment.configure_and_build(root, "windows-x64-release", "web"),
                    root / "build/windows-x64-release",
                )
        self.assertEqual(
            run.call_args_list[0].args[0],
            [
                "cmake", "--preset", "windows-x64-release", "-DBUILD_TESTING=OFF",
                "-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER=",
            ],
        )

    def test_existing_build_reconfiguration_targets_candidate_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = self.make_build(root)
            cache = build / "CMakeCache.txt"
            cache.write_text(
                cache.read_text(encoding="utf-8")
                + "CMAKE_TOOLCHAIN_FILE:FILEPATH=C:/vcpkg/toolchain.cmake\n"
                + "VCPKG_OVERLAY_PORTS:STRING=C:/work/ports\n",
                encoding="utf-8",
            )
            with patch.object(dev_environment.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
                self.assertTrue(dev_environment.configure_build(root, "windows-x64-release", build, None))
            command = run.call_args.args[0]
            self.assertEqual(command[:5], ["cmake", "-S", str(root), "-B", str(build)])
            self.assertIn("-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/toolchain.cmake", command)

    def test_newest_web_seed_requires_clean_same_commit_and_uses_newest_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "current"
            older = Path(directory) / "older"
            newer = Path(directory) / "newer"
            for worktree in (root, older, newer):
                (worktree / "web/dist").mkdir(parents=True)
                (worktree / "web/dist/index.html").write_text("ok", encoding="utf-8")
            os.utime(older / "web/dist/index.html", (1, 1))
            os.utime(newer / "web/dist/index.html", (2, 2))
            with patch.object(dev_environment, "registered_worktrees", return_value=[root, older, newer]), \
                 patch.object(dev_environment, "current_commit", return_value="same"), \
                 patch.object(dev_environment, "web_worktree_is_clean", return_value=True):
                self.assertEqual(dev_environment.newest_web_seed(root), newer / "web/dist/index.html")
            with patch.object(dev_environment, "registered_worktrees", return_value=[root, newer]), \
                 patch.object(dev_environment, "current_commit", return_value="same"), \
                 patch.object(dev_environment, "web_worktree_is_clean", side_effect=lambda item: item != newer):
                self.assertIsNone(dev_environment.newest_web_seed(root))

    def test_seed_web_assets_copies_current_frontend_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "current"
            source = Path(directory) / "source"
            (root / "web/src").mkdir(parents=True)
            (root / "web/src/main.js").write_text("source", encoding="utf-8")
            (source / "web/dist").mkdir(parents=True)
            (source / "web/dist/index.html").write_text("output", encoding="utf-8")
            freshness = iter([True, False])
            builder = type("Builder", (), {"web_build_is_stale": staticmethod(lambda _web, _index: next(freshness))})
            with patch.object(dev_environment, "newest_web_seed", return_value=source / "web/dist/index.html"):
                self.assertTrue(dev_environment.seed_web_assets(root, builder))
            self.assertEqual((root / "web/dist/index.html").read_text(encoding="utf-8"), "output")

    def test_sccache_stats_parses_real_nested_shape(self):
        payload = {
            "stats": {
                "cache_hits": {"counts": {"C/C++": 7}, "adv_counts": {"C/C++": 2}},
                "cache_misses": {"counts": {"C/C++": 3}, "adv_counts": {}},
                "cache_errors": {"counts": {"C/C++": 1}, "adv_counts": {}},
                "cache_read_errors": 2,
                "cache_write_errors": 1,
                "dist_errors": 4,
            }
        }
        completed = subprocess.CompletedProcess([], 0, stdout=json.dumps(payload))
        with patch.object(dev_environment.subprocess, "run", return_value=completed):
            self.assertEqual(
                dev_environment.sccache_stats(Path("sccache")),
                {"hits": 9, "misses": 3, "errors": 8},
            )

    def test_main_refreshes_build_and_web_assets_before_web_start(self):
        candidate = dev_environment.BuildCandidate(
            Path("C:/work/build"), Path("C:/work"), Path("C:/work/build/acecode.exe")
        )
        args = type("Args", (), {"target": "web", "build_dir": None, "yes": False, "dry_run": False, "extra": []})()
        with patch.object(dev_environment, "parse_args", return_value=args), \
             patch.object(dev_environment, "project_root", return_value=Path("C:/work")), \
             patch.object(dev_environment, "find_sccache", return_value=None), \
             patch.object(dev_environment, "find_compatible_build", return_value=candidate), \
             patch.object(dev_environment, "cache_state_changed", return_value=False), \
             patch.object(dev_environment, "build_target", return_value=True) as build, \
             patch.object(dev_environment, "refresh_web_assets", return_value=True) as refresh, \
             patch.object(dev_environment, "launch_surface", return_value=0) as launch:
            self.assertEqual(dev_environment.main(), 0)
        build.assert_called_once_with(Path("C:/work"), candidate.build_dir, "web", None)
        refresh.assert_called_once_with(Path("C:/work"))
        launch.assert_called_once_with(Path("C:/work"), "web", candidate, False, [])

    def test_confirmation_eof_refuses_configuration(self):
        with patch.object(dev_environment.sys.stdin, "isatty", return_value=True), \
             patch("builtins.input", side_effect=EOFError):
            self.assertFalse(dev_environment.ask_to_build("windows-x64-release", "web", False))

    def test_main_retries_failed_sccache_build_without_cache(self):
        candidate = dev_environment.BuildCandidate(Path("C:/work/build"), Path("C:/work"), Path("C:/work/build/acecode.exe"))
        args = type("Args", (), {"target": "web", "build_dir": None, "yes": False, "dry_run": False, "extra": []})()
        cache = Path("C:/tools/sccache.exe")
        with patch.object(dev_environment, "parse_args", return_value=args), \
             patch.object(dev_environment, "project_root", return_value=Path("C:/work")), \
             patch.object(dev_environment, "find_sccache", return_value=cache), \
             patch.object(dev_environment, "sccache_disabled_for_build", return_value=False), \
             patch.object(dev_environment, "find_compatible_build", return_value=candidate), \
             patch.object(dev_environment, "cache_state_changed", return_value=False), \
             patch.object(dev_environment, "build_target", side_effect=[False, True]) as build, \
             patch.object(dev_environment, "configure_build", return_value=True) as configure, \
             patch.object(dev_environment, "refresh_web_assets", return_value=True), \
             patch.object(dev_environment, "launch_surface", return_value=0):
            self.assertEqual(dev_environment.main(), 0)
        self.assertEqual(build.call_args_list[0].args, (Path("C:/work"), candidate.build_dir, "web", cache))
        self.assertEqual(build.call_args_list[1].args, (Path("C:/work"), candidate.build_dir, "web", None))
        configure.assert_called_once_with(Path("C:/work"), "windows-x64-release", candidate.build_dir, None)

    def test_web_launch_forwards_the_build_and_isolated_runtime_directory(self):
        candidate = dev_environment.BuildCandidate(
            Path("C:/work/build"), Path("C:/work"), Path("C:/work/build/acecode.exe")
        )
        with patch.object(dev_environment, "worktree_runtime_dir", return_value=Path("C:/work/.acecode/dev-run/test")):
            result = dev_environment.launch_surface(Path("C:/work"), "web", candidate, dry_run=True, extra=[])
        self.assertEqual(result, 0)

    def test_windows_target_launchers_auto_approve_initial_configuration(self):
        for target in ("web", "desktop", "tui"):
            wrapper = (ROOT / "scripts" / f"dev_{target}.bat").read_text(encoding="utf-8")
            self.assertIn(f'dev_environment.py" {target} --yes %*', wrapper)

    @unittest.skipUnless(os.name == "nt", "Windows batch wrapper")
    def test_batch_wrapper_delegates_to_python(self):
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
