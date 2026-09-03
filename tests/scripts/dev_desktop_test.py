#!/usr/bin/env python3
"""Regression tests for scripts/dev_desktop.py."""

from __future__ import annotations

import ast
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
