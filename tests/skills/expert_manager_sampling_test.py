from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "assets/seed/skills/expert-management/expert-manager/scripts"))

from batch_create import create_one
from validate_expert import ValidationResult, validate_sampling


class ExpertManagerSamplingTests(unittest.TestCase):
    def test_batch_preserves_sampling_in_installed_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "experts"
            sampling = {"temperature": 0, "top_p": 0.95, "top_k": 20, "min_p": 0}
            item = {
                "name": "sampling-expert", "type": "agent", "displayName": "Sampler",
                "profession": "Reviewer", "displayDescription": "Reviews code",
                "instructions": "Review carefully", "quickPrompts": ["Review this"],
                "sampling": sampling,
            }
            ok, message = create_one(item, root)
            self.assertTrue(ok, message)
            manifest = json.loads((root / item["name"] / "expert.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["sampling"], sampling)
            item["name"] = "invalid-expert"
            item["sampling"] = {"top_k": 1.5}
            ok, message = create_one(item, root)
            self.assertFalse(ok)
            self.assertIn("sampling.top_k", message)
            self.assertFalse((root / item["name"]).exists())

    def test_validator_accepts_inheritance_and_rejects_bad_fields(self) -> None:
        for value in [None, {}, {"temperature": 0, "top_p": None, "min_p": 0}]:
            result = ValidationResult()
            validate_sampling(value, result)
            self.assertTrue(result.is_valid, result.summary())
        for value in [[], {"temperature": True}, {"temperature": "1"}, {"top_p": 0},
                      {"top_k": 1.5}, {"top_k": 10 ** 400}, {"min_p": 2}, {"typo": 1}]:
            result = ValidationResult()
            validate_sampling(value, result)
            self.assertFalse(result.is_valid, str(value))


if __name__ == "__main__":
    unittest.main()
