"""Opt-in Windows Desktop smoke: the main WebView is probed, reloaded and recovered on a real WebView2."""
import http.server
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

SCENARIOS = ("healthy", "nav-cancelled", "hung", "hung-user", "browser-exit")


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        page = b"<!doctype html><title>ACECode webview health test</title>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(page)))
        self.end_headers()
        self.wfile.write(page)

    def log_message(self, *_args):
        pass


def run_scenario(binary, url, scenario):
    # Every scenario gets its own profile: the load counter lives in localStorage,
    # and the browser-exit scenario kills that profile's browser process.
    with tempfile.TemporaryDirectory(prefix="ace-webview-health-", ignore_cleanup_errors=True) as folder:
        env = dict(os.environ)
        env["WEBVIEW2_USER_DATA_FOLDER"] = str(Path(folder) / "profile")
        result = subprocess.run(
            [str(binary), url, scenario],
            env=env, capture_output=True, text=True, timeout=90,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        return result.returncode


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    scenarios = sys.argv[2:] or SCENARIOS
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        url = f"http://127.0.0.1:{server.server_port}/"
        failed = [name for name in scenarios if run_scenario(binary, url, name) != 0]
        if failed:
            print("failed scenarios: " + ", ".join(failed), file=sys.stderr)
        return 1 if failed else 0
    finally:
        server.shutdown()
        worker.join()
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
