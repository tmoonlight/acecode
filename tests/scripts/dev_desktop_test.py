#!/usr/bin/env python3
"""Regression tests for scripts/dev_desktop.py."""

from __future__ import annotations

import ast
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "scripts" / "dev_desktop.py"


def load_dev_desktop_module():
    spec = importlib.util.spec_from_file_location("dev_desktop_under_test", SCRIPT_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


dev_desktop = load_dev_desktop_module()


class DevDesktopTest(unittest.TestCase):
    def test_default_cli_lists_all_platform_builds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / 'CMakeLists.txt').write_text('project(acecode)', encoding='utf-8')
            (repo / 'src').mkdir()
            (repo / 'web').mkdir()
            app = repo / 'build' / 'macos-arm64-release' / 'ACECode.app'
            app.mkdir(parents=True)
            windows = repo / 'build' / 'Release' / 'acecode-desktop.exe'
            windows.parent.mkdir()
            windows.write_bytes(b'')
            def invoke(*args):
                result = subprocess.run(
                    [sys.executable, '-B', str(SCRIPT_PATH), '--root', str(repo), '--list', *args],
                    capture_output=True, text=True, encoding='utf-8',
                    env={**os.environ, 'PYTHONIOENCODING': 'utf-8'},
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout
            output = invoke()
            self.assertIn('ACECode.app', output)
            self.assertIn('acecode-desktop.exe', output)
            explicit = invoke('--build-dir', 'build/macos-arm64-release')
            self.assertIn('ACECode.app', explicit)
            self.assertNotIn('acecode-desktop.exe', explicit)

    def test_source_is_parseable_with_python_38_grammar(self) -> None:
        source = SCRIPT_PATH.read_text(encoding="utf-8")
        ast.parse(source, filename=str(SCRIPT_PATH), feature_version=(3, 8))
        self.assertIn("from __future__ import annotations", source)

    def test_web_build_freshness_includes_manifest_public_and_source(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            web = Path(root_text)
            (web / "dist").mkdir()
            (web / "src").mkdir()
            (web / "public").mkdir()
            index = web / "dist" / "index.html"
            index.write_text("dist", encoding="utf-8")
            old = index.stat().st_mtime - 10
            os.utime(index, (old, old))

            for relative in ("src/app.js", "public/icon.svg", "package.json",
                             "pnpm-lock.yaml", "vite.config.js"):
                path = web / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative, encoding="utf-8")
                self.assertTrue(dev_desktop.web_build_is_stale(web, index), relative)
                older = old - 10
                os.utime(path, (older, older))

            self.assertFalse(dev_desktop.web_build_is_stale(web, index))

    def test_finds_direct_and_nested_multiconfig_desktop_builds(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            build = Path(root_text) / "build"
            build.mkdir()
            direct = build / "acecode-desktop.exe"
            nested = build / "vs-audit" / "Release" / "acecode-desktop.exe"
            direct.write_bytes(b"")
            nested.parent.mkdir(parents=True)
            nested.write_bytes(b"")

            builds = {path.resolve() for path in dev_desktop.find_desktop_builds(build)}
            self.assertIn(direct.resolve(), builds)
            self.assertIn(nested.resolve(), builds)

    def test_external_build_path_falls_back_to_absolute_display(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            root = Path(root_text)
            repo = root / "repo"
            external = root / "external" / "Release" / "acecode-desktop.exe"
            repo.mkdir()
            external.parent.mkdir(parents=True)
            external.write_bytes(b"")

            self.assertEqual(
                dev_desktop.display_path(external, repo),
                str(external.resolve()),
            )

    def test_ansi_color_code_has_single_terminator(self) -> None:
        original = dev_desktop._COLOR
        try:
            dev_desktop._COLOR = True
            self.assertEqual(dev_desktop._c("x", "36"), "\x1b[36mx\x1b[0m")
        finally:
            dev_desktop._COLOR = original

    def test_desktop_instance_identity_matches_web_runtime_naming(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            repo = Path(root_text) / "my.worktree"
            # 目录名需要真实存在，否则 git 调用无从谈起
            repo.mkdir()
            identity = "0123456789abcdef0123456789abcdef01234567"
            with mock.patch.object(dev_desktop, "desktop_instance_identity",
                                   wraps=dev_desktop.desktop_instance_identity):
                actual = self._identity_with_commit(repo, identity)
            self.assertEqual(actual, "my.worktree_0123456789ab")
            self.assertEqual(re.sub(r"[^A-Za-z0-9_.-]", "-", actual), actual)

    def test_desktop_instance_identity_sanitizes_unsafe_worktree_names(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            repo = Path(root_text) / "fix dev launcher+identity"
            repo.mkdir()
            actual = self._identity_with_commit(repo, "a" * 40)
        self.assertEqual(actual, "fix_dev_launcher_identity_aaaaaaaaaaaa")

    def _identity_with_commit(self, repo: Path, commit: str) -> str:
        """`desktop_instance_identity` 依赖 git，测试里替换掉 commit 查询。"""
        import dev_environment

        with mock.patch.object(dev_environment, "current_commit", return_value=commit):
            return dev_desktop.desktop_instance_identity(repo)

    def test_falls_back_to_worktree_name_when_commit_is_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            repo = Path(root_text) / "detached"
            repo.mkdir()
            self.assertEqual(
                self._identity_with_commit(repo, None),
                "detached_detached",
            )

    def test_desktop_environment_injects_process_level_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            repo = Path(root_text) / "acecode"
            repo.mkdir()
            web = repo / "web" / "dist"
            web.mkdir(parents=True)

            with mock.patch.dict(os.environ, {"ACECODE_TEST_SENTINEL": "kept"}, clear=False):
                environment, identity = dev_desktop.desktop_environment(repo, web)

            self.assertEqual(identity, "acecode_acecode")
            self.assertEqual(environment["ACECODE_DESKTOP_INSTANCE_ID"], identity)
            self.assertEqual(environment["ACECODE_DESKTOP_ALLOW_MULTIPLE_INSTANCES"], "1")
            self.assertEqual(environment["ACECODE_DEV_WEB_DIR"], str(web.resolve()))
            # 继承父进程环境，而不是另起一份干净环境
            self.assertEqual(environment["ACECODE_TEST_SENTINEL"], "kept")

    def test_desktop_environment_is_identical_across_repeated_calls(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            repo = Path(root_text) / "acecode"
            repo.mkdir()
            web = repo / "web" / "dist"
            web.mkdir(parents=True)

            first, first_identity = dev_desktop.desktop_environment(repo, web)
            second, second_identity = dev_desktop.desktop_environment(repo, web)
            self.assertEqual(first_identity, second_identity)
            self.assertEqual(first["ACECODE_DESKTOP_INSTANCE_ID"],
                             second["ACECODE_DESKTOP_INSTANCE_ID"])

    def test_launch_desktop_passes_overrides_to_child_process(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            repo = Path(root_text) / "acecode"
            repo.mkdir()
            web = repo / "web" / "dist"
            web.mkdir(parents=True)
            binary = repo / "build" / "acecode-desktop.exe"
            binary.parent.mkdir()
            binary.write_bytes(b"")

            environment, identity = dev_desktop.desktop_environment(repo, web)
            with mock.patch.object(dev_desktop.subprocess, "Popen") as popen:
                dev_desktop.launch_desktop(binary, web, instance_id=identity,
                                           environment=environment)

            self.assertEqual(popen.call_count, 1)
            _, kwargs = popen.call_args
            passed = kwargs["env"]
            self.assertEqual(passed["ACECODE_DESKTOP_INSTANCE_ID"], identity)
            self.assertEqual(passed["ACECODE_DESKTOP_ALLOW_MULTIPLE_INSTANCES"], "1")
            self.assertEqual(passed["ACECODE_DEV_WEB_DIR"], str(web.resolve()))
            self.assertEqual(popen.call_args.args[0], [str(binary)])

    def test_launch_desktop_never_writes_user_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as root_text:
            repo = Path(root_text) / "acecode"
            repo.mkdir()
            web = repo / "web" / "dist"
            web.mkdir(parents=True)
            binary = repo / "build" / "acecode-desktop.exe"
            binary.parent.mkdir()
            binary.write_bytes(b"")

            with mock.patch.object(dev_desktop.subprocess, "Popen"), \
                    mock.patch.object(dev_desktop, "desktop_instance_identity",
                                      return_value="acecode-deadbeef1234"):
                dev_desktop.launch_desktop(binary, web, project_root=repo)

            # 开发期覆盖只走进程环境，任何配置文件都不应被创建或改写
            leftovers = [path for path in repo.rglob("*") if path.is_file()
                         and path != binary]
            self.assertEqual(leftovers, [])


if __name__ == "__main__":
    unittest.main()
