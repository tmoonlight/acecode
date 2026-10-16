import contextlib
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import subprocess
import unittest
import zlib
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("dev_environment", ROOT / "scripts/dev_environment.py")
dev_environment = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = dev_environment
spec.loader.exec_module(dev_environment)


class DevEnvironmentTest(unittest.TestCase):
    def test_permission_denied_does_not_mark_live_pid_as_dead(self):
        for error, expected in ((PermissionError(errno.EPERM, "denied"), True),
                                (ProcessLookupError(errno.ESRCH, "missing"), False)):
            with self.subTest(error=error), patch.object(dev_environment.os, "name", "posix"), \
                 patch.object(dev_environment.os, "kill", side_effect=error):
                self.assertEqual(dev_environment.pid_is_alive(12345), expected)

    def test_windows_probe_keeps_inaccessible_processes_and_uses_full_width_handles(self):
        import ctypes
        from ctypes import wintypes
        kernel = Mock()
        kernel.OpenProcess.return_value = None
        for error, expected in ((5, True), (87, False), (6, True)):
            with self.subTest(error=error), patch.object(ctypes, "WinDLL", return_value=kernel, create=True), \
                 patch.object(ctypes, "get_last_error", return_value=error, create=True):
                self.assertEqual(dev_environment.windows_pid_is_alive(12345), expected)
        self.assertIs(kernel.OpenProcess.restype, wintypes.HANDLE)
        kernel.OpenProcess.return_value = 0x123456789
        kernel.GetExitCodeProcess.return_value = False
        with patch.object(ctypes, "WinDLL", return_value=kernel, create=True):
            self.assertTrue(dev_environment.windows_pid_is_alive(12345))
        kernel.CloseHandle.assert_called_once_with(0x123456789)

    def test_prune_preserves_invalid_pid_and_linked_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory) / "runtime"
            runtime.mkdir()
            for value in ("0", "-1", "invalid"):
                (runtime / "daemon.pid").write_text(value, encoding="utf-8")
                pruned, skipped = dev_environment.prune_runtime_directories([runtime])
                self.assertEqual(pruned, [])
                self.assertEqual(len(skipped), 1)
                self.assertTrue(runtime.is_dir())
            (runtime / "daemon.pid").write_text("12345", encoding="utf-8")
            with patch.object(Path, "is_symlink", return_value=True), \
                 patch.object(dev_environment, "pid_is_alive", return_value=False):
                self.assertFalse(dev_environment.classify_runtime_directory(runtime)[0])

    def test_explicit_port_range_is_validated_before_binding(self):
        with patch.object(dev_environment, "port_is_available") as probe:
            for port in ("0", "-1", "65536"):
                self.assertIsNone(dev_environment.select_runtime_port(Path("runtime"), ["--port", port]))
        probe.assert_not_called()

    def setUp(self):
        environment = patch.object(dev_environment, "ensure_windows_environment", return_value=True)
        environment.start()
        self.addCleanup(environment.stop)

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
        (build / dev_environment.native_executable_name("acecode")).chmod(0o755)
        if desktop:
            (build / dev_environment.native_executable_name("acecode-desktop")).touch()
            (build / dev_environment.native_executable_name("acecode-desktop")).chmod(0o755)
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
        root = Path("work").resolve()
        other = Path("other").resolve()
        output = f"worktree {root.as_posix()}\nHEAD abc\n\nworktree {other.as_posix()}\nHEAD def\n"
        completed = subprocess.CompletedProcess([], 0, stdout=output)
        with patch.object(dev_environment.subprocess, "run", return_value=completed):
            self.assertEqual(dev_environment.registered_worktrees(root), [root, other])

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
        root = Path("C:/work")
        executable = Path("C:/work/build/acecode.exe")
        with patch.object(dev_environment.os, "name", "nt"):
            command = dev_environment.tui_command(root, executable)
        self.assertEqual(command, [str(executable)])

    def test_runtime_directory_is_scoped_to_worktree(self):
        root = Path("C:/worktrees/my project")
        with patch.object(dev_environment, "current_commit", return_value="abcdef1234567890"):
            runtime = dev_environment.worktree_runtime_dir(root)
        self.assertEqual(runtime, root / ".acecode/dev_run/my_project_abcdef123456")

    def test_noninteractive_target_selection_fails(self):
        with patch.object(dev_environment.sys, "stdin", None):
            self.assertIsNone(dev_environment.choose_target(None))

    def test_existing_legacy_runtime_and_logs_remain_discoverable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "old-project"
            legacy = root / ".acecode/dev-run/old-project-abcdef123456"
            legacy.mkdir(parents=True)
            (legacy / "daemon-worker.log").write_text("old worker", encoding="utf-8")
            (legacy / "daemon-startup.log").write_text("old startup", encoding="utf-8")
            with patch.object(dev_environment, "current_commit", return_value="abcdef1234567890"):
                self.assertEqual(dev_environment.worktree_runtime_dir(root), legacy)
            self.assertFalse((root / ".acecode/dev_run").exists())
            detail = dev_environment.worker_failure_detail(legacy, "failed")
            self.assertIn(str(legacy / "daemon-worker.log"), detail)
            self.assertIn(str(legacy / "daemon-startup.log"), detail)

    def test_sccache_discovery_prefers_usable_path_and_has_install_hint(self):
        completed = subprocess.CompletedProcess([], 0, stdout="sccache 1.0")
        with patch.object(dev_environment.shutil, "which", return_value="C:/tools/sccache.exe"), \
             patch.object(Path, "is_file", return_value=True), \
             patch.object(dev_environment.subprocess, "run", return_value=completed):
            self.assertEqual(dev_environment.find_sccache(), Path("C:/tools/sccache.exe").resolve())
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
            root = Path(directory).resolve() / "current"
            older = Path(directory).resolve() / "older"
            newer = Path(directory).resolve() / "newer"
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

    def test_main_starts_quick_web_without_rebuilding_existing_daemon(self):
        candidate = dev_environment.BuildCandidate(
            Path("C:/work/build"), Path("C:/work"), Path("C:/work/build/acecode.exe")
        )
        args = type("Args", (), {"target": "web", "build_dir": None, "yes": False, "dry_run": False, "extra": []})()
        with patch.object(dev_environment, "parse_args", return_value=args), \
             patch.object(dev_environment, "project_root", return_value=Path("C:/work")), \
             patch.object(dev_environment, "find_compatible_build", return_value=candidate), \
             patch.object(dev_environment, "launch_quick_web", return_value=0) as launch, \
             patch.object(dev_environment, "build_target") as build, \
             patch.object(dev_environment, "refresh_web_assets") as refresh:
            self.assertEqual(dev_environment.main(), 0)
        launch.assert_called_once_with(Path("C:/work"), candidate, [])
        build.assert_not_called()
        refresh.assert_not_called()

    def test_main_embedded_mode_builds_and_refreshes_assets(self):
        candidate = dev_environment.BuildCandidate(
            Path("C:/work/build"), Path("C:/work"), Path("C:/work/build/acecode.exe")
        )
        args = type("Args", (), {"target": "web", "build_dir": None, "yes": False, "dry_run": False, "extra": ["--embedded"]})()
        with patch.object(dev_environment, "parse_args", return_value=args), \
             patch.object(dev_environment, "project_root", return_value=Path("C:/work")), \
             patch.object(dev_environment, "find_sccache", return_value=None), \
             patch.object(dev_environment, "find_compatible_build", return_value=candidate), \
             patch.object(dev_environment, "cache_state_changed", return_value=False), \
             patch.object(dev_environment, "build_target", return_value=True) as build, \
             patch.object(dev_environment, "refresh_web_assets", return_value=True) as refresh, \
             patch.object(dev_environment, "launch_surface", return_value=0) as launch:
            self.assertEqual(dev_environment.main(), 0)
        build.assert_called_once_with(Path("C:/work"), candidate.build_dir, "web", None, None)
        refresh.assert_called_once_with(Path("C:/work"))
        launch.assert_called_once_with(Path("C:/work"), "web", candidate, False, [])

    def test_quick_web_reuses_healthy_daemon_and_passes_its_port_to_vite(self):
        root = Path("C:/work")
        candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
        run_dir = root / ".acecode/dev-run/test"
        with patch.object(dev_environment, "selected_web_runtime_dir", return_value=run_dir), \
             patch.object(dev_environment, "daemon_is_healthy", return_value=True), \
             patch.object(dev_environment, "daemon_port", return_value=38123), \
             patch.object(dev_environment, "launch_vite", return_value=0) as vite, \
             patch.object(dev_environment, "start_quick_web_daemon") as start:
            self.assertEqual(dev_environment.launch_quick_web(root, candidate, []), 0)
        vite.assert_called_once_with(root, 38123, run_dir, [])
        start.assert_not_called()

    def test_quick_web_starts_on_the_identity_derived_port(self):
        root = Path("C:/work")
        candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
        run_dir = root / ".acecode/dev-run/work-abcdef123456"
        expected = dev_environment.derived_port("work-abcdef123456")
        with patch.object(dev_environment, "selected_web_runtime_dir", return_value=run_dir), \
             patch.object(dev_environment, "daemon_is_healthy", return_value=False), \
             patch.object(Path, "exists", return_value=False), \
             patch.object(dev_environment, "port_is_available", return_value=True), \
             patch.object(dev_environment, "start_quick_web_daemon", return_value=expected) as start, \
             patch.object(dev_environment, "launch_vite", return_value=0) as vite:
            self.assertEqual(dev_environment.launch_quick_web(root, candidate, []), 0)
        self.assertEqual([call.args[3] for call in start.call_args_list], [expected])
        vite.assert_called_once_with(root, expected, run_dir, [])

    def test_derived_port_is_stable_across_processes(self):
        """crc32 keeps the port stable; the built-in hash() would not."""
        import zlib
        for identity in ("acecode-f33bbdb9cef6", "my-project-abcdef123456", "dev-0123456789ab"):
            expected = 28080 + zlib.crc32(identity.encode("utf-8")) % 201
            self.assertEqual(dev_environment.derived_port(identity), expected)
            self.assertTrue(28080 <= expected <= 28280)
        self.assertNotEqual(
            dev_environment.derived_port("acecode-aaaaaaaaaaaa"),
            dev_environment.derived_port("acecode-bbbbbbbbbbbb"),
        )

    def _rotation_order(self, identity: str) -> list[int]:
        start = dev_environment.derived_port(identity)
        return [(start - dev_environment.PORT_RANGE_START + offset) % dev_environment.PORT_RANGE_COUNT
                + dev_environment.PORT_RANGE_START
                for offset in range(dev_environment.PORT_RANGE_COUNT)]

    def test_derived_port_succeeds_linearly_and_wraps_in_range(self):
        run_dir = Path("C:/work/.acecode/dev-run/work-abcdef123456")
        order = self._rotation_order(run_dir.name)
        busy = set(order)
        free_port = order[-1]
        busy.discard(free_port)
        probed: list[int] = []

        def available(port):
            probed.append(port)
            return port not in busy

        with patch.object(dev_environment, "port_is_available", side_effect=available):
            self.assertEqual(dev_environment.select_runtime_port(run_dir, []), free_port)
        self.assertEqual(probed, order)

    def test_derived_port_probes_in_order_without_waiting(self):
        run_dir = Path("C:/work/.acecode/dev-run/work-000000000000")
        order = self._rotation_order(run_dir.name)
        busy = set(order[:3])
        probed: list[int] = []

        def available(port):
            probed.append(port)
            return port not in busy

        with patch.object(dev_environment, "port_is_available", side_effect=available):
            self.assertEqual(dev_environment.select_runtime_port(run_dir, []), order[3])
        self.assertEqual(probed, order[:4])

    def test_exhausted_port_range_reports_failure(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/work-000000000000"
        with patch.object(dev_environment, "port_is_available", return_value=False) as probe:
            with (contextlib.redirect_stderr(io.StringIO()) as output):
                self.assertIsNone(dev_environment.select_runtime_port(run_dir, []))
        self.assertIn("28080-28280", output.getvalue())
        self.assertEqual(probe.call_count, 201)

    def test_explicit_port_is_used_when_free(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/work-000000000000"
        with patch.object(dev_environment, "port_is_available", return_value=True) as probe:
            self.assertEqual(dev_environment.select_runtime_port(run_dir, ["--port", "39001"]), 39001)
        probe.assert_called_once_with(39001)

    def test_explicit_busy_port_fails_without_adjacent_probing(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/work-000000000000"
        with patch.object(dev_environment, "port_is_available", return_value=False) as probe:
            with contextlib.redirect_stderr(io.StringIO()) as output:
                self.assertIsNone(dev_environment.select_runtime_port(run_dir, ["--port", "39001"]))
        probe.assert_called_once_with(39001)
        self.assertIn("39001", output.getvalue())

    def test_quick_web_recovers_a_runtime_whose_pid_is_dead(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
            run_dir = root / ".acecode/dev-run/work-000000000000"
            run_dir.mkdir(parents=True)
            (run_dir / "daemon.pid").write_text("4242", encoding="utf-8")
            with patch.object(dev_environment, "selected_web_runtime_dir", return_value=run_dir), \
                 patch.object(dev_environment, "daemon_is_healthy", return_value=False), \
                 patch.object(dev_environment, "pid_is_alive", return_value=False) as alive, \
                 patch.object(dev_environment.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as stop, \
                 patch.object(dev_environment, "port_is_available", return_value=True), \
                 patch.object(dev_environment, "start_quick_web_daemon", return_value=28080) as start, \
                 patch.object(dev_environment, "launch_vite", return_value=0):
                self.assertEqual(dev_environment.launch_quick_web(root, candidate, []), 0)
            alive.assert_called_once_with(4242)
            self.assertEqual(stop.call_args.args[0][1:4], ["daemon", "stop", f"--run-dir={run_dir.as_posix()}"])
            start.assert_called_once()

    def test_quick_web_never_stops_a_live_pid(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
            run_dir = root / ".acecode/dev-run/work-000000000000"
            run_dir.mkdir(parents=True)
            (run_dir / "daemon.pid").write_text("4242", encoding="utf-8")
            with patch.object(dev_environment, "selected_web_runtime_dir", return_value=run_dir), \
                 patch.object(dev_environment, "daemon_is_healthy", return_value=False), \
                 patch.object(dev_environment, "pid_is_alive", return_value=True), \
                 patch.object(dev_environment.subprocess, "run") as stop, \
                 patch.object(dev_environment, "start_quick_web_daemon") as start:
                with contextlib.redirect_stderr(io.StringIO()) as output:
                    self.assertEqual(dev_environment.launch_quick_web(root, candidate, []), 1)
            stop.assert_not_called()
            start.assert_not_called()
            self.assertIn("stop it manually", output.getvalue())
            self.assertIn("still alive", output.getvalue())

    def test_unreadable_pid_file_skips_self_healing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
            run_dir = root / ".acecode/dev-run/work-000000000000"
            run_dir.mkdir(parents=True)
            (run_dir / "daemon.pid").write_text("not-a-pid", encoding="utf-8")
            with patch.object(dev_environment, "selected_web_runtime_dir", return_value=run_dir), \
                 patch.object(dev_environment, "daemon_is_healthy", return_value=False), \
                 patch.object(dev_environment, "pid_is_alive") as alive, \
                 patch.object(dev_environment.subprocess, "run") as stop, \
                 patch.object(dev_environment, "start_quick_web_daemon") as start:
                with contextlib.redirect_stderr(io.StringIO()) as output:
                    self.assertEqual(dev_environment.launch_quick_web(root, candidate, []), 1)
            alive.assert_not_called()
            stop.assert_not_called()
            start.assert_not_called()
            self.assertIn("unreadable pid file", output.getvalue())

    def test_prune_reports_pruned_skipped_and_totals(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dead = root / ".acecode/dev-run/work-000000000000"
            live = root / ".acecode/dev-run/work-111111111111"
            unknown = root / ".acecode/dev-run/other-222222222222"
            for path, pid in ((dead, "100"), (live, "200"), (unknown, None)):
                path.mkdir(parents=True)
                if pid:
                    (path / "daemon.pid").write_text(pid, encoding="utf-8")
            legacy = root / ".acecode-dev-run"
            legacy.mkdir()
            (legacy / "daemon.pid").write_text("300", encoding="utf-8")

            def alive(pid):
                return pid == 200

            with patch.object(dev_environment, "pid_is_alive", side_effect=alive):
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(dev_environment.prune_runtime_tree(root), 0)
            report = output.getvalue()
            self.assertFalse(dead.exists())
            self.assertFalse(legacy.exists())
            self.assertTrue(live.exists())
            self.assertTrue(unknown.exists())
            self.assertIn("dead pid=100", report)
            self.assertIn("dead pid=300", report)
            self.assertIn("pid 200 is alive", report)
            self.assertIn("no readable daemon.pid", report)
            self.assertIn("Total: pruned 2, skipped 2", report)

    def test_prune_dry_run_deletes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dead = root / ".acecode/dev-run/work-000000000000"
            dead.mkdir(parents=True)
            (dead / "daemon.pid").write_text("100", encoding="utf-8")
            with patch.object(dev_environment, "pid_is_alive", return_value=False):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(dev_environment.prune_runtime_tree(root, dry_run=True), 0)
            self.assertTrue(dead.exists())

    def test_startup_prunes_only_dead_sibling_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "work"
            current = root / ".acecode/dev-run/work-000000000000"
            stale = root / ".acecode/dev-run/work-111111111111"
            foreign = root / ".acecode/dev-run/other-222222222222"
            for path, pid in ((current, "1"), (stale, "2"), (foreign, "3")):
                path.mkdir(parents=True)
                (path / "daemon.pid").write_text(pid, encoding="utf-8")

            def alive(pid):
                return pid in (1, 3)

            with patch.object(dev_environment, "pid_is_alive", side_effect=alive):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(dev_environment.prune_worktree_runtime_dirs(root, current), 1)
            self.assertTrue(current.exists())
            self.assertFalse(stale.exists())
            self.assertTrue(foreign.exists())

    def test_worker_log_header_and_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory)
            command = ["C:/build/acecode.exe", "daemon", "--foreground"]
            dev_environment.open_worker_log(run_dir, command).close()
            log = run_dir / "daemon_worker.log"
            self.assertIn("spawn: C:/build/acecode.exe daemon --foreground", log.read_text(encoding="utf-8"))
            log.write_text("x" * (dev_environment.WORKER_LOG_MAX_BYTES + 1), encoding="utf-8")
            dev_environment.open_worker_log(run_dir, command).close()
            self.assertLess(log.stat().st_size, dev_environment.WORKER_LOG_MAX_BYTES)
            self.assertTrue((run_dir / "daemon_worker.log.1").exists())

    def test_spawn_redirects_worker_output_into_the_runtime_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "work"
            run_dir = root / ".acecode/dev-run/test"
            command = ["C:/build/acecode.exe", "daemon", "--foreground", "--port=28080"]
            worker = type("Worker", (), {"poll": staticmethod(lambda: None)})()
            with patch.object(dev_environment.subprocess, "Popen", return_value=worker) as popen:
                self.assertIs(dev_environment.spawn_daemon_worker(root, run_dir, command), worker)
            options = popen.call_args.kwargs
            self.assertEqual(popen.call_args.args[0], command)
            self.assertEqual(options["stderr"], subprocess.STDOUT)
            self.assertEqual(Path(options["stdout"].name), run_dir / "daemon_worker.log")
            self.assertTrue((run_dir / "daemon_worker.log").is_file())

    def test_worker_exit_fails_before_the_health_timeout(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/test"
        candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
        worker = type("Worker", (), {"poll": staticmethod(lambda: 3)})()
        started = time.monotonic()
        self.assertFalse(dev_environment.daemon_is_healthy(root, candidate, run_dir, timeout_seconds=30, worker=worker))
        self.assertLess(time.monotonic() - started, 5)

    def test_start_failure_points_at_both_logs(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/test"
        candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
        worker = type("Worker", (), {"poll": staticmethod(lambda: 3)})()
        with patch.object(dev_environment, "spawn_daemon_worker", return_value=worker), \
             patch.object(dev_environment, "daemon_port", return_value=28080):
            with contextlib.redirect_stderr(io.StringIO()) as output:
                self.assertIsNone(dev_environment.start_quick_web_daemon(root, candidate, run_dir, 28080))
        self.assertIn("daemon_worker.log", output.getvalue())
        self.assertIn("daemon_startup.log", output.getvalue())

    def test_pid_is_alive_reads_the_process_table(self):
        self.assertTrue(dev_environment.pid_is_alive(os.getpid()))
        self.assertFalse(dev_environment.pid_is_alive(0))
        self.assertFalse(dev_environment.pid_is_alive(999983))

    def test_runtime_pid_reads_and_tolerates_junk(self):
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory)
            (run_dir / "daemon.pid").write_text("123", encoding="utf-8")
            self.assertEqual(dev_environment.runtime_pid(run_dir), 123)
            (run_dir / "daemon.pid").write_text("junk", encoding="utf-8")
            self.assertIsNone(dev_environment.runtime_pid(run_dir))
            self.assertIsNone(dev_environment.runtime_pid(run_dir / "missing"))

    def test_prune_target_runs_without_a_build(self):
        args = type("Args", (), {"target": "prune", "build_dir": None, "yes": False, "dry_run": False, "extra": []})()
        with patch.object(dev_environment, "parse_args", return_value=args), \
             patch.object(dev_environment, "project_root", return_value=Path("C:/work")), \
             patch.object(dev_environment, "prune_runtime_tree", return_value=0) as prune:
            self.assertEqual(dev_environment.main(), 0)
        prune.assert_called_once_with(Path("C:/work"), dry_run=False)

    def test_quick_daemon_start_requires_a_healthy_runtime(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/test"
        candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
        worker = type("Worker", (), {"poll": staticmethod(lambda: None)})()
        with patch.object(dev_environment, "spawn_daemon_worker", return_value=worker), \
             patch.object(dev_environment, "daemon_is_healthy", return_value=False), \
             patch.object(dev_environment, "daemon_port", return_value=28080), \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertIsNone(dev_environment.start_quick_web_daemon(root, candidate, run_dir, 28080))

    def test_healthy_daemon_requires_authenticated_identity_match(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/test"
        candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
        with tempfile.TemporaryDirectory() as temp:
            runtime = Path(temp) / "runtime"
            runtime.mkdir()
            (runtime / "daemon.pid").write_text("42", encoding="utf-8")
            (runtime / "daemon.port").write_text("38123", encoding="utf-8")
            (runtime / "token").write_text("secret", encoding="utf-8")
            response = type("Response", (), {
                "__enter__": lambda self: self,
                "__exit__": lambda self, *args: None,
                "read": lambda self: b'{"pid": 41, "port": 38123}',
            })()
            with patch.object(dev_environment.urllib.request, "urlopen", return_value=response) as urlopen:
                self.assertFalse(dev_environment.daemon_is_healthy(root, candidate, runtime))
            urlopen.assert_called_once()

    def test_quick_daemon_starts_foreground_worker_with_isolated_runtime(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/test"
        candidate = dev_environment.BuildCandidate(root / "build", root, root / "build/acecode.exe")
        worker = type("Worker", (), {"poll": staticmethod(lambda: None)})()
        with patch.object(dev_environment, "spawn_daemon_worker", return_value=worker) as spawn, \
             patch.object(dev_environment, "daemon_is_healthy", return_value=True), \
             patch.object(dev_environment, "daemon_port", return_value=28080):
            self.assertEqual(dev_environment.start_quick_web_daemon(root, candidate, run_dir, 28080), 28080)
        self.assertEqual(spawn.call_args.args, (root, run_dir, [
            str(candidate.executable), "daemon", "--foreground",
            "--cwd=C:/work", "--run-dir=C:/work/.acecode/dev-run/test", "--port=28080",
        ]))

    def test_launch_vite_passes_daemon_credentials_only_to_child_environment(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/test"
        builder = type("Builder", (), {"ensure_node_and_pnpm": staticmethod(lambda: ("node", "pnpm"))})
        with patch.object(dev_environment, "load_web_builder", return_value=builder), \
             patch.object(dev_environment, "daemon_token", return_value="test-token"), \
             patch.object(Path, "is_dir", return_value=True), \
             patch.object(dev_environment.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            self.assertEqual(dev_environment.launch_vite(root, 38123, run_dir, ["--host", "127.0.0.1"]), 0)
        command = run.call_args.args[0]
        environment = run.call_args.kwargs["env"]
        self.assertEqual(command, ["pnpm", "dev", "--host", "127.0.0.1"])
        self.assertEqual(environment["ACECODE_DAEMON_PORT"], "38123")
        self.assertEqual(environment["ACECODE_DAEMON_TOKEN"], "test-token")

    def test_vite_options_strip_daemon_runtime_arguments(self):
        self.assertEqual(
            dev_environment.vite_options(["--run-dir", "runtime", "--host", "127.0.0.1"]),
            ["--host", "127.0.0.1"],
        )
        self.assertEqual(
            dev_environment.vite_options(["--run-dir=runtime", "--port", "5174"]),
            [],
        )
        self.assertEqual(
            dev_environment.vite_options(["--port=28080", "--run-dir=runtime", "--open"]),
            ["--open"],
        )

    def test_launch_vite_refuses_missing_daemon_token(self):
        root = Path("C:/work")
        run_dir = root / ".acecode/dev-run/test"
        with patch.object(dev_environment, "daemon_token", return_value=None), \
             patch.object(dev_environment, "load_web_builder") as builder:
            self.assertEqual(dev_environment.launch_vite(root, 38123, run_dir, []), 1)
        builder.assert_not_called()

    def test_missing_quick_daemon_refuses_noninteractive_build_without_authorization(self):
        args = type("Args", (), {"target": "web", "build_dir": None, "yes": False, "dry_run": False, "extra": []})()
        with patch.object(dev_environment, "parse_args", return_value=args), \
             patch.object(dev_environment, "project_root", return_value=Path("C:/work")), \
             patch.object(dev_environment, "find_compatible_build", return_value=None), \
             patch.object(dev_environment, "default_preset", return_value="windows-x64-release"), \
             patch.object(dev_environment.sys, "stdin", None), \
             patch.object(dev_environment, "configure_and_build") as build:
            self.assertEqual(dev_environment.main(), 1)
        build.assert_not_called()

    def test_confirmation_eof_refuses_configuration(self):
        stdin = type("Stdin", (), {"isatty": staticmethod(lambda: True)})()
        with patch.object(dev_environment.sys, "stdin", stdin), \
             patch("builtins.input", side_effect=EOFError):
            self.assertFalse(dev_environment.ask_to_build("windows-x64-release", "web", False))

    def test_main_retries_failed_sccache_build_without_cache(self):
        candidate = dev_environment.BuildCandidate(Path("C:/work/build"), Path("C:/work"), Path("C:/work/build/acecode.exe"))
        args = type("Args", (), {"target": "web", "build_dir": None, "yes": False, "dry_run": False, "extra": ["--embedded"]})()
        cache = Path("C:/tools/sccache.exe")
        with patch.object(dev_environment, "parse_args", return_value=args), \
             patch.object(dev_environment, "project_root", return_value=Path("C:/work")), \
             patch.object(dev_environment, "find_sccache", return_value=cache), \
             patch.object(dev_environment, "default_preset", return_value="windows-x64-release"), \
             patch.object(dev_environment, "sccache_disabled_for_build", return_value=False), \
             patch.object(dev_environment, "find_compatible_build", return_value=candidate), \
             patch.object(dev_environment, "cache_state_changed", return_value=False), \
             patch.object(dev_environment, "build_target", side_effect=[False, True]) as build, \
             patch.object(dev_environment, "configure_build", return_value=True) as configure, \
             patch.object(dev_environment, "refresh_web_assets", return_value=True), \
             patch.object(dev_environment, "launch_surface", return_value=0):
            self.assertEqual(dev_environment.main(), 0)
        self.assertEqual(build.call_args_list[0].args, (Path("C:/work"), candidate.build_dir, "web", cache, None))
        self.assertEqual(build.call_args_list[1].args, (Path("C:/work"), candidate.build_dir, "web", None, None))
        configure.assert_called_once_with(Path("C:/work"), "windows-x64-release", candidate.build_dir, None)

    def test_web_launch_forwards_the_build_and_isolated_runtime_directory(self):
        candidate = dev_environment.BuildCandidate(
            Path("C:/work/build"), Path("C:/work"), Path("C:/work/build/acecode.exe")
        )
        with patch.object(dev_environment, "worktree_runtime_dir", return_value=Path("C:/work/.acecode/dev-run/test")):
            result = dev_environment.launch_surface(Path("C:/work"), "web", candidate, dry_run=True, extra=[])
        self.assertEqual(result, 0)

    def test_windows_web_launcher_does_not_auto_approve_native_build(self):
        wrapper = (ROOT / "scripts/dev_web.bat").read_text(encoding="utf-8")
        self.assertIn('dev_environment.py" web %*', wrapper)
        self.assertNotIn('dev_environment.py" web --yes %*', wrapper)

    def test_windows_other_target_launchers_auto_approve_initial_configuration(self):
        for target in ("desktop", "tui"):
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
