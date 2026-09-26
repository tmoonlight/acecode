"""Pure fixture checks; never start the startup probe or a GUI."""

import copy
import json
from pathlib import Path, PureWindowsPath
import unittest

import capture_windows as capture


HERE = Path(__file__).resolve().parent


class ResolvedFixturePath:
    def __init__(self, value):
        self.value = PureWindowsPath(value)

    def resolve(self):
        return self.value


class CaptureFixtureTest(unittest.TestCase):
    def archived(self, scenario):
        return json.loads((HERE / (scenario + ".json")).read_text("utf-8"))

    def test_cwd_hash_preserves_non_ascii_case_in_utf8_bytes(self):
        # Python str.lower changes U+00C9; the original C++ byte fold does not.
        upper = ResolvedFixturePath("C:/Users/\u00c9va/Temp/probe/user/workspace")
        lower = ResolvedFixturePath("C:/Users/\u00e9va/Temp/probe/user/workspace")
        self.assertEqual("a7132c056e8f0a63", capture.cwd_hash(upper))
        self.assertEqual("b9b81a91d65e7dc3", capture.cwd_hash(lower))
        self.assertNotEqual(capture.cwd_hash(upper), capture.cwd_hash(lower))

    def test_cwd_hash_still_normalizes_ascii_case_and_separators(self):
        path = ResolvedFixturePath("c:/USERS/\u00c9va/TEMP/PROBE/USER/WORKSPACE/")
        self.assertEqual("a7132c056e8f0a63", capture.cwd_hash(path))

    def test_archived_four_scenarios_pass_without_mutating_observations(self):
        for scenario in capture.SCENARIOS:
            with self.subTest(scenario=scenario):
                snapshot = self.archived(scenario)
                original = copy.deepcopy(snapshot)
                capture.validate_snapshot(snapshot, scenario)
                self.assertEqual(original, snapshot)

    def test_extra_messages_fail_for_every_scenario(self):
        unexpected = self.archived("copilot-unauthenticated")["messages"][0]
        for scenario in capture.SCENARIOS:
            with self.subTest(scenario=scenario):
                snapshot = self.archived(scenario)
                snapshot["messages"].append(copy.deepcopy(unexpected))
                snapshot["total_messages"] += 1
                with self.assertRaises(ValueError):
                    capture.validate_snapshot(snapshot, scenario)

    def test_truncated_or_inconsistent_message_counts_fail(self):
        for count in (0, 2, 17, True, 3.0):
            with self.subTest(count=count):
                snapshot = self.archived("resume")
                snapshot["total_messages"] = count
                with self.assertRaises(ValueError):
                    capture.validate_snapshot(snapshot, "resume")

    def test_resume_missing_reordered_or_changed_messages_fail(self):
        snapshots = []
        missing = self.archived("resume")
        missing["messages"] = missing["messages"][-1:]
        missing["messages"][0]["content"] = "Session " + capture.SESSION_ID + " not found."
        missing["total_messages"] = 1
        snapshots.append(missing)
        reordered = self.archived("resume")
        reordered["messages"][0], reordered["messages"][1] = reordered["messages"][1], reordered["messages"][0]
        snapshots.append(reordered)
        for index in range(3):
            changed = self.archived("resume")
            changed["messages"][index]["content"] += " changed"
            snapshots.append(changed)
        for snapshot in snapshots:
            with self.subTest(messages=snapshot["messages"]):
                with self.assertRaises(ValueError):
                    capture.validate_snapshot(snapshot, "resume")

    def test_async_result_or_wrong_mcp_count_is_not_a_pending_snapshot(self):
        for scenario, content in (("copilot-unauthenticated", "Authenticated (saved token)."),
                                  ("mcp-configured", "[MCP] Starting 2 server(s) in the background.")):
            with self.subTest(scenario=scenario):
                snapshot = self.archived(scenario)
                snapshot["messages"][0]["content"] = content
                with self.assertRaises(ValueError):
                    capture.validate_snapshot(snapshot, scenario)

    def test_schema_and_observation_checkpoint_are_required(self):
        for key, value in (("schema_version", 2), ("schema_version", True),
                           ("limit", 15), ("limit", 16.0),
                           ("checkpoint", "after-event-loop"), ("scenario", "resume")):
            with self.subTest(key=key, value=value):
                snapshot = self.archived("ordinary")
                snapshot[key] = value
                with self.assertRaises(ValueError):
                    capture.validate_snapshot(snapshot, "ordinary")
        with self.assertRaises(ValueError):
            capture.validate_snapshot(self.archived("ordinary"), "unknown")

    def test_missing_or_unknown_fields_and_wrong_types_are_rejected(self):
        for location in ("root", "message"):
            for operation in ("missing", "unknown"):
                with self.subTest(location=location, operation=operation):
                    snapshot = self.archived("resume")
                    target = snapshot if location == "root" else snapshot["messages"][0]
                    if operation == "missing":
                        target.pop(next(iter(target)))
                    else:
                        target["future_field"] = "must not be ignored"
                    with self.assertRaises(ValueError):
                        capture.validate_snapshot(snapshot, "resume")
        for key, value in (("is_tool", 0), ("expanded", 0), ("ask_result", 0),
                           ("summary", {}), ("hunks", []), ("display_override", "changed"),
                           ("compact_notice_id", "unexpected"), ("compact_notice_complete", True)):
            with self.subTest(key=key):
                snapshot = self.archived("resume")
                snapshot["messages"][0][key] = value
                with self.assertRaises(ValueError):
                    capture.validate_snapshot(snapshot, "resume")
        for invalid in (None, [], "not a snapshot"):
            with self.assertRaises(ValueError):
                capture.validate_snapshot(invalid, "ordinary")


if __name__ == "__main__":
    unittest.main()
