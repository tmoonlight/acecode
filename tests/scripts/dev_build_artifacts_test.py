import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("dev_build_artifacts", ROOT / "scripts/dev_build_artifacts.py")
artifacts = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = artifacts
spec.loader.exec_module(artifacts)


class DevBuildArtifactsTest(unittest.TestCase):
    def test_finds_root_and_two_level_artifacts_without_deep_tree_scan(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            direct = build / "acecode.exe"
            nested = build / "preset" / "Release" / "acecode.exe"
            too_deep = build / "a" / "b" / "c" / "acecode.exe"
            for path in (direct, nested, too_deep):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            self.assertEqual(artifacts.find_named_artifacts(build, ["acecode.exe"]), [direct, nested])


if __name__ == "__main__":
    unittest.main()
