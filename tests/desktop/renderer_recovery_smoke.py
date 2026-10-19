"""Opt-in Windows Desktop smoke: the main WebView reloads after its renderer runs out of memory."""
import http.server
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        page = b"<!doctype html><title>ACECode renderer recovery test</title>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(page)))
        self.end_headers()
        self.wfile.write(page)

    def log_message(self, *_args):
        pass


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="ace-renderer-recovery-", ignore_cleanup_errors=True) as folder:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            env = dict(os.environ)
            env["WEBVIEW2_USER_DATA_FOLDER"] = str(Path(folder) / "profile")
            # A small V8 old-space limit makes the renderer hit the same
            # out-of-memory exit within seconds instead of after days.
            env["WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"] = "--js-flags=--max-old-space-size=64"
            result = subprocess.run(
                [str(binary), f"http://127.0.0.1:{server.server_port}/"],
                env=env, capture_output=True, text=True, timeout=90,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            return result.returncode
        finally:
            server.shutdown()
            worker.join()
            server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
