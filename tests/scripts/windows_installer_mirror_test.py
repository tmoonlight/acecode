import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("mirror", ROOT / "scripts/mirror_windows_installers.py")
mirror = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mirror)


class InstallerMirrorTest(unittest.TestCase):
    def test_missing_corrupt_or_invalid_installer_prevents_any_deployment(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            assets, destination = root / "assets", root / "server"
            assets.mkdir()
            sums = []
            for arch in ("x64", "arm64"):
                for name in (f"ACECode-1.2.3-windows-{arch}-setup.exe", f"acecode-windows-{arch}.zip"):
                    path = assets / name
                    path.write_bytes(b"fixture")
                    sums.append(f"{mirror.digest(path)}  {name}")
            (assets / "SHA256SUMS.txt").write_text("\n".join(sums), encoding="utf-8")
            target = assets / "ACECode-1.2.3-windows-arm64-setup.exe"
            with patch.object(mirror, "verify_setup"):
                self.assertEqual(len(mirror.preflight(assets, "1.2.3")), 2)
                for content in (None, b"", b"corrupt"):
                    if content is None:
                        target.unlink()
                    else:
                        target.write_bytes(content)
                    with self.assertRaises(ValueError):
                        mirror.publish(assets, "1.2.3", destination, "http://unused.invalid")
                    self.assertFalse(destination.exists())
                target.write_bytes(b"fixture")
            with patch.object(mirror, "verify_setup", side_effect=ValueError("invalid PE")):
                with self.assertRaises(ValueError):
                    mirror.publish(assets, "1.2.3", destination, "http://unused.invalid")
                self.assertFalse(destination.exists())

    def test_download_publication_preserves_manifest_and_checks_all_four_urls(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source.exe"
            source.write_bytes(b"verified-installer")
            destination = root / "server"
            destination.mkdir()
            manifest = destination / "aceupdate.json"
            manifest.write_bytes(b'{"latest":"1.2.3","releases":[]}')
            original = manifest.read_bytes()
            definitions = [{"source": str(source), "file": f"versioned-{arch}.exe",
                            "alias": f"latest-{arch}.exe", "size": source.stat().st_size,
                            "sha256": mirror.digest(source)} for arch in ("x64", "arm64")]
            with patch.object(mirror, "preflight", return_value=definitions), patch.object(mirror, "verify_public") as public:
                mirror.publish(root, "1.2.3", destination, "http://downloads.invalid/")
                self.assertEqual(public.call_count, 4)
            self.assertEqual(manifest.read_bytes(), original)
            for item in definitions:
                for name in (item["file"], item["alias"]):
                    self.assertEqual((destination / name).read_bytes(), source.read_bytes())


if __name__ == "__main__":
    unittest.main()
