from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = (
    REPO_ROOT
    / "assets"
    / "seed"
    / "skills"
    / "expert-management"
    / "expert-manager"
    / "scripts"
)
sys.path.insert(0, str(SCRIPTS))

from batch_create import create_one  # noqa: E402
from init_expert import agent_manifest  # noqa: E402
from validate_expert import MAX_AVATAR_BYTES, validate_expert  # noqa: E402


def write_agent_package(package: Path, state_avatars: object) -> None:
    (package / "agents").mkdir(parents=True)
    (package / "agents" / "lead.md").write_text(
        "---\nname: lead\n---\n\nWork carefully.\n", encoding="utf-8"
    )
    manifest = {
        "name": package.name,
        "version": "1.0.0",
        "expertType": "agent",
        "displayName": "State Expert",
        "profession": "Tester",
        "displayDescription": "Tests state avatars.",
        "quickPrompts": ["Test this"],
        "defaultInitPrompt": "Test this",
        "agentName": "lead",
        "agents": [{"id": "lead", "path": "agents/lead.md"}],
        "stateAvatars": state_avatars,
    }
    (package / "expert.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


class ExpertManagerStateAvatarTests(unittest.TestCase):
    def test_validator_accepts_png_and_preserves_gif_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            package = Path(raw) / "state-expert"
            (package / "avatars").mkdir(parents=True)
            gif_bytes = b"GIF89a\x01\x00\x01\x00animated"
            (package / "avatars" / "working.gif").write_bytes(gif_bytes)
            (package / "avatars" / "idle.png").write_bytes(
                b"\x89PNG\r\n\x1a\nidle"
            )
            write_agent_package(
                package,
                {
                    "working": "avatars/working.gif",
                    "idle": "avatars/idle.png",
                },
            )

            result = validate_expert(package, require_installed=False)

            self.assertTrue(result.is_valid, result.summary())
            self.assertEqual(
                (package / "avatars" / "working.gif").read_bytes(), gif_bytes
            )

    def test_validator_rejects_unknown_missing_traversal_and_type(self) -> None:
        cases = (
            ({"future": "avatars/future.png"}, "unknown stateAvatars"),
            ({"working": "avatars/missing.png"}, "does not exist"),
            ({"working": "../outside.png"}, "escapes"),
            ({"working": "avatars/working.svg"}, "unsupported image type"),
        )
        for states, expected in cases:
            with self.subTest(states=states), tempfile.TemporaryDirectory() as raw:
                package = Path(raw) / "invalid-state"
                (package / "avatars").mkdir(parents=True)
                (package.parent / "outside.png").write_bytes(b"outside")
                (package / "avatars" / "working.svg").write_text(
                    "<svg/>", encoding="utf-8"
                )
                write_agent_package(package, states)
                result = validate_expert(package, require_installed=False)
                self.assertFalse(result.is_valid)
                self.assertIn(expected, result.summary())

    def test_validator_rejects_oversized_state_asset(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            package = Path(raw) / "oversized-state"
            (package / "avatars").mkdir(parents=True)
            oversized = package / "avatars" / "working.gif"
            with oversized.open("wb") as output:
                output.seek(MAX_AVATAR_BYTES)
                output.write(b"\0")
            write_agent_package(
                package, {"working": "avatars/working.gif"}
            )

            result = validate_expert(package, require_installed=False)

            self.assertFalse(result.is_valid)
            self.assertIn("exceeds", result.summary())

    def test_batch_copies_original_state_assets_and_rejects_unknown_keys(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            base = Path(raw)
            root = base / "experts"
            assets = base / "input"
            (assets / "avatars").mkdir(parents=True)
            gif_bytes = b"GIF89a\x01\x00\x01\x00batch-animation"
            (assets / "avatars" / "working.gif").write_bytes(gif_bytes)
            item = {
                "name": "batch-state",
                "type": "agent",
                "displayName": "Batch State",
                "profession": "Tester",
                "displayDescription": "Batch state avatar test.",
                "instructions": "Test batch creation.",
                "quickPrompts": ["Test batch"],
                "stateAvatars": {"working": "avatars/working.gif"},
            }

            ok, message = create_one(item, root, assets)

            self.assertTrue(ok, message)
            installed = root / "batch-state"
            self.assertEqual(
                (installed / "avatars" / "working.gif").read_bytes(), gif_bytes
            )
            manifest = json.loads((installed / "expert.json").read_text("utf-8"))
            self.assertEqual(
                manifest["stateAvatars"]["working"], "avatars/working.gif"
            )

            bad = {**item, "name": "bad-input", "unexpected": True}
            ok, message = create_one(bad, root, assets)
            self.assertFalse(ok)
            self.assertIn("unknown expert definition keys", message)

    def test_batch_rejects_invalid_state_assets(self) -> None:
        cases = (
            ({"future": "avatars/future.png"}, "unknown 'stateAvatars'"),
            ({"working": "../outside.png"}, "must stay inside"),
            ({"working": "avatars/missing.png"}, "does not exist"),
            ({"working": "avatars/working.svg"}, "unsupported image type"),
        )
        with tempfile.TemporaryDirectory() as raw:
            base = Path(raw)
            root = base / "experts"
            assets = base / "input"
            (assets / "avatars").mkdir(parents=True)
            (base / "outside.png").write_bytes(b"outside")
            (assets / "avatars" / "working.svg").write_text(
                "<svg/>", encoding="utf-8"
            )
            for index, (states, expected) in enumerate(cases):
                with self.subTest(states=states):
                    item = {
                        "name": f"invalid-batch-{index}",
                        "type": "agent",
                        "displayName": "Invalid Batch",
                        "profession": "Tester",
                        "displayDescription": "Reject invalid assets.",
                        "instructions": "Reject invalid assets.",
                        "quickPrompts": ["Validate"],
                        "stateAvatars": states,
                    }
                    ok, message = create_one(item, root, assets)
                    self.assertFalse(ok)
                    self.assertIn(expected, message)

    def test_initializer_does_not_fabricate_state_paths(self) -> None:
        manifest = agent_manifest("new-expert")
        self.assertNotIn("stateAvatars", manifest)
        self.assertNotIn("avatar", manifest)


if __name__ == "__main__":
    unittest.main()
