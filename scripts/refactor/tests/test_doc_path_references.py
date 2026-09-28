import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_doc_paths import inspect


class DocPathReferencesTest(unittest.TestCase):
    def test_exact_example_registration_does_not_mask_a_new_broken_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "scripts/refactor").mkdir(parents=True)
            (root / "guide.md").write_text("src/example.cpp src/example.cpp src/broken.cpp")
            policy = {"references": [{
                "file": "guide.md", "path": "src/example.cpp", "kind": "example",
                "count": 1, "note": "Example user project response",
            }]}
            (root / "scripts/refactor/doc_path_references.json").write_text(json.dumps(policy))
            report = inspect(root, ["guide.md"])
            self.assertEqual(1, len(report["non_repository_references"]))
            self.assertEqual(["src/example.cpp", "src/broken.cpp"],
                             [item["path"] for item in report["findings"]])


if __name__ == "__main__":
    unittest.main()
