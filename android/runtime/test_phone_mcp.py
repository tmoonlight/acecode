"""Exercise the same stdio entry point that the packaged Linux daemon starts."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

RELAY = Path(__file__).resolve().parents[1] / "app/src/main/assets/phone/phone_mcp.py"


class PhoneMcpTest(unittest.TestCase):
    def test_discovery_notifications_and_missing_android_endpoint(self):
        requests = [
            {"id": 1, "method": "initialize", "params": {"protocolVersion": "2025-03-26"}},
            {"method": "notifications/initialized"},
            {"id": 2, "method": "tools/list"},
            {"id": 3, "method": "tools/call", "params": {"name": "tap", "arguments": {"id": 1, "screen": "stale"}}},
            {"id": 4, "method": "ping"},
        ]
        lines = "".join(json.dumps({"jsonrpc": "2.0", **r}) + "\n" for r in requests)
        output = subprocess.run([sys.executable, str(RELAY)], input=lines, capture_output=True,
                                encoding="utf-8", check=True, timeout=5)
        replies = [json.loads(line) for line in output.stdout.splitlines()]
        self.assertEqual([r["id"] for r in replies], [1, 2, 3, 4])
        self.assertEqual(replies[0]["result"]["protocolVersion"], "2025-03-26")
        tools = {t["name"]: t for t in replies[1]["result"]["tools"]}
        self.assertEqual(set(tools), {"open", "apps", "status", "screen", "tap"})
        self.assertEqual(set(tools["tap"]["inputSchema"]["required"]), {"id", "screen"})
        self.assertFalse(tools["tap"]["annotations"]["readOnlyHint"])
        self.assertFalse(tools["open"]["annotations"]["readOnlyHint"])
        self.assertTrue(tools["screen"]["annotations"]["readOnlyHint"])
        self.assertTrue(replies[2]["result"]["isError"])
        self.assertEqual(replies[3]["result"], {})


if __name__ == "__main__":
    unittest.main()
