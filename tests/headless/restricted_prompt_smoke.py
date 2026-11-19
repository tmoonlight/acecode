"""Run a real headless binary and inspect every request sent to a loopback provider."""

import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import tempfile
import threading


def verify(binary):
    requests = []

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append(body)
            if body.get("stream"):
                chunks = [
                    {"choices": [{"index": 0, "delta": {"content": "Fixture answer"}}]},
                    {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
                ]
                data = "".join("data: " + json.dumps(c) + "\n\n" for c in chunks) + "data: [DONE]\n\n"
                content_type = "text/event-stream"
            else:
                data = json.dumps({"choices": [{"message": {"role": "assistant", "content": "Fixture answer"}, "finish_reason": "stop"}]})
                content_type = "application/json"
            encoded = data.encode()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

    with ThreadingHTTPServer(("127.0.0.1", 0), Provider) as server, tempfile.TemporaryDirectory() as tmp:
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            home = Path(tmp)
            data = home / ".acecode"
            data.mkdir()
            workspace = home / "workspace"
            workspace.mkdir()
            (workspace / "AGENTS.md").write_text("unrelated project directions", encoding="utf-8")
            prompt = home / "role.txt"
            prompt.write_text("Operator role only.", encoding="utf-8")
            profile = dict(name="test", provider="openai", base_url=f"http://127.0.0.1:{server.server_port}/v1", api_key="fixture", model="fixture-model", context_window=128000)
            config = dict(provider="openai", openai=profile, saved_models=[profile], default_model_name="test", session_title={"enabled": True}, upgrade={"auto_check": False})
            (data / "config.json").write_text(json.dumps(config), encoding="utf-8")
            env = dict(os.environ, HOME=tmp, USERPROFILE=tmp, NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")
            for option, text in [("--session-id", "First question"), ("--resume", "Second question")]:
                result = subprocess.run([str(binary), "-p", option, "prompt-smoke", "--model", "test", "--tool-policy", "deny-by-default", "--system-prompt-file", str(prompt), "--max-turns", "2"], input=text, cwd=workspace, env=env, capture_output=True, text=True, timeout=45)
                assert result.returncode == 0, result.stderr
                assert "Fixture answer" in result.stdout, result.stdout
            assert len(requests) == 2, f"expected two conversation requests, got {len(requests)}"
            for request in requests:
                assert not request.get("tools")
                system = [m["content"] for m in request["messages"] if m["role"] in ("system", "developer")]
                assert system == ["Operator role only."], system
                assert "unrelated project directions" not in json.dumps(request)
            assert "First question" in json.dumps(requests[-1]["messages"])
        finally:
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    verify(parser.parse_args().binary.resolve())
    print("Verified two headless turns with an empty toolset and a fully replaced prompt.")
