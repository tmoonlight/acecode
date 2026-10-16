import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("release_assets", ROOT / "scripts/verify_release_assets.py")
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)


class ReleaseAssetsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.names = assets.required_asset_names("1.2.3")
        for index, name in enumerate(self.names):
            path = self.root / f"artifact-{index}" / name
            path.parent.mkdir()
            path.write_bytes(b"package")

    def test_complete_release_requires_all_seventeen_downloads(self):
        selected = assets.verify_release_assets(self.root, "1.2.3")
        self.assertEqual(len(selected), 17)
        for arch in ("x64", "arm64"):
            self.assertIn(f"acecode-linux-deepin-{arch}.tar.gz", self.names)
        self.assertIn("acecode-linux-armv7.tar.gz", self.names)
        for arch in ("x64", "arm64"):
            self.assertIn(f"ACECode-1.2.3-macos-{arch}.pkg", self.names)

    def test_retired_deepin_armv7_archive_is_rejected(self):
        # 场景:1.0.7 之后不再发布 Deepin ARMv7(用户决定),但产物目录里混进了一个
        # acecode-linux-deepin-armv7.tar.gz(旧 run、恢复构建残留)。
        # 期望:它不在必需清单里,出现时按「意外资产」拒绝发布,而不是被悄悄传上 Release。
        self.assertNotIn("acecode-linux-deepin-armv7.tar.gz", self.names)
        (self.root / "acecode-linux-deepin-armv7.tar.gz").write_bytes(b"package")
        with self.assertRaisesRegex(ValueError, "Unexpected release asset: acecode-linux-deepin-armv7.tar.gz"):
            assets.verify_release_assets(self.root, "1.2.3")

    def test_every_missing_or_empty_asset_blocks_publication(self):
        for name in self.names:
            with self.subTest(name=name):
                path = next(self.root.rglob(name))
                path.unlink()
                with self.assertRaisesRegex(ValueError, "found 0"):
                    assets.verify_release_assets(self.root, "1.2.3")
                path.touch()
                with self.assertRaisesRegex(ValueError, "Empty release asset"):
                    assets.verify_release_assets(self.root, "1.2.3")
                path.write_bytes(b"package")

    def test_old_linux_archives_cannot_replace_deepin_packages(self):
        for arch in ("x64", "arm64"):
            path = next(self.root.rglob(f"acecode-linux-deepin-{arch}.tar.gz"))
            path.rename(path.with_name(f"acecode-linux-old-{arch}.tar.gz"))
        with self.assertRaises(ValueError) as error:
            assets.verify_release_assets(self.root, "1.2.3")
        self.assertIn("acecode-linux-deepin-x64.tar.gz; found 0", str(error.exception))
        self.assertIn("Unexpected release asset: acecode-linux-old-x64.tar.gz", str(error.exception))

    def test_duplicates_unsigned_and_wrong_version_assets_are_rejected(self):
        duplicate = self.root / self.names[0]
        duplicate.write_bytes(b"different package")
        with self.assertRaisesRegex(ValueError, "found 2"):
            assets.verify_release_assets(self.root, "1.2.3")
        duplicate.unlink()
        for name in ("ACECode-1.2.3-macos-arm64-unsigned.pkg",
                     "ACECode-1.2.3-macos-arm64-update-unsigned.zip",
                     "ACECode-1.2.2-macos-arm64.pkg"):
            with self.subTest(name=name):
                unexpected = self.root / name
                unexpected.write_bytes(b"untrusted")
                with self.assertRaisesRegex(ValueError, "Unexpected release asset"):
                    assets.verify_release_assets(self.root, "1.2.3")
                unexpected.unlink()

    def test_version_mismatch_or_invalid_version_is_rejected(self):
        with self.assertRaises(ValueError):
            assets.verify_release_assets(self.root, "1.2.4")
        for version in ("", "../1.2.3", "1.2.3/path", "v1.2.3"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                assets.required_asset_names(version)

    def test_non_package_artifacts_do_not_affect_runtime_completeness(self):
        for name in ("acecode.debug", "acecode.pdb", "index.html"):
            (self.root / name).write_bytes(b"auxiliary")
        self.assertEqual(len(assets.verify_release_assets(self.root, "1.2.3")), 17)


if __name__ == "__main__":
    unittest.main()
