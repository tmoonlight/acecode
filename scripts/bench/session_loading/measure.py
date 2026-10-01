"""Measure HTTP and optionally browser loading against a disposable daemon."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
from urllib.request import Request, urlopen
from urllib.error import URLError


@contextmanager
def daemon(executable, profile, static_dir=None):
    manifest = json.loads((profile / "manifest.json").read_text(encoding="utf-8"))
    if not manifest.get("synthetic") or Path(manifest["profile"]).resolve() != profile.resolve():
        raise ValueError("Not a generated benchmark profile")
    env = os.environ.copy()
    for name, suffix in (("USERPROFILE", ""), ("HOME", ""), ("APPDATA", "AppData/Roaming"),
                         ("LOCALAPPDATA", "AppData/Local"), ("TEMP", "temp"), ("TMP", "temp")):
        target = profile / suffix
        target.mkdir(parents=True, exist_ok=True)
        env[name] = str(target)
    # Prevent a caller's process-level data directory override escaping the fixture.
    for key in list(env):
        if key.startswith("ACECODE_") and any(word in key for word in ("DATA", "HOME", "RUN", "TOKEN", "CONFIG")):
            env.pop(key)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    runtime = profile / ("run-" + str(time.time_ns()))
    runtime.mkdir()
    command = [str(executable.resolve()), "daemon", "--foreground", f"--cwd={manifest['cwd']}",
               f"--run-dir={runtime}", f"--port={port}"]
    if static_dir:
        command.append(f"--static-dir={static_dir.resolve()}")
    with (runtime / "worker.log").open("wb") as log:
        worker = subprocess.Popen(command, env=env, cwd=manifest["cwd"], stdout=log, stderr=log,
                                  creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    try:
        token = ""
        base = f"http://127.0.0.1:{port}"
        for _ in range(300):
            if worker.poll() is not None:
                raise RuntimeError(f"Daemon exited {worker.returncode}; see {runtime / 'worker.log'}")
            try:
                token = (runtime / "token").read_text(encoding="utf-8").strip()
                with urlopen(Request(base + "/api/health", headers={"X-ACECode-Token": token}), timeout=1) as response:
                    health = json.load(response)
                    if health.get("pid") == worker.pid:
                        break
            except (OSError, ValueError, URLError):
                time.sleep(.1)
        else:
            raise RuntimeError("Isolated daemon health timed out")
        yield base, token, manifest
    finally:
        if worker.poll() is None:
            worker.terminate()
            try:
                worker.wait(timeout=10)
            except subprocess.TimeoutExpired:
                worker.kill()
                worker.wait()


def request(base, token, path, method="GET", body=None):
    start = time.perf_counter()
    data = json.dumps(body or {}).encode() if method == "POST" else None
    with urlopen(Request(base + path, data=data, method=method,
                         headers={"X-ACECode-Token": token, "Content-Type": "application/json"}), timeout=120) as response:
        payload = response.read()
        return {"ms": round((time.perf_counter() - start) * 1000, 3), "bytes": len(payload), "status": response.status}


def browser_measure(base, token, manifest, duration=30, session_index=0, check_paging=False):
    from playwright.sync_api import sync_playwright
    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 1440, "height": 1000})
        ws_frames = []
        errors = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        def record_frame(payload):
            try:
                ws_frames.append({"time": time.perf_counter(), "type": json.loads(payload).get("type", "")})
            except (ValueError, TypeError):
                pass
        page.on("websocket", lambda socket: socket.on("framesent", record_frame))
        page.add_init_script("""window.__sessionBench = {longTasks: []};
            new PerformanceObserver(list => window.__sessionBench.longTasks.push(
              ...list.getEntries().map(e => ({start: e.startTime, ms: e.duration}))
            )).observe({type: 'longtask', buffered: true});""")
        page.goto(base + "/?token=" + token, wait_until="domcontentloaded")
        sid = manifest["sessions"][session_index]["id"]
        row = page.locator(f'[data-desktop-session-id="{sid}"]')
        row.first.wait_for(state="visible", timeout=60000)
        page.evaluate("performance.clearResourceTimings()")
        start = page.evaluate("performance.now()")
        row.first.click()
        page.locator('[data-chat-row="true"]').first.wait_for(state="visible", timeout=120000)
        first_paint = page.evaluate("performance.now()") - start
        steady_started = time.perf_counter()
        page.wait_for_timeout(duration * 1000)
        resources = page.evaluate("""performance.getEntriesByType('resource')
          .filter(e => e.name.includes('/api/')).map(e => ({
            path: new URL(e.name).pathname + new URL(e.name).search,
            start: e.startTime, ms: e.duration, bytes: e.decodedBodySize
          }))""")
        history = [entry for entry in resources if "/messages?" in entry["path"]]
        steady = [entry for entry in resources if start + first_paint <= entry["start"] < start + first_paint + duration * 1000]
        steady_lists = [entry for entry in steady if entry["path"] == "/api/sessions" or "/sessions?" in entry["path"] or "scope=no-workspace" in entry["path"]]
        result = {"first_content_ms": round(first_paint, 3), "history_requests": len(history),
                  "history_bytes": sum(e["bytes"] for e in history), "resources": resources,
                  "steady_seconds": duration, "steady_requests": len(steady),
                  "steady_status_subscriptions": sum(frame["type"] == "status_subscribe" and steady_started <= frame["time"] < steady_started + duration for frame in ws_frames),
                  "steady_list_requests": len(steady_lists), "steady_list_bytes": sum(e["bytes"] for e in steady_lists),
                  "long_tasks": page.evaluate("window.__sessionBench.longTasks")}
        result["page_errors"] = errors
        if check_paging:
            import re
            result["paging_checks"] = []
            for attempt in range(12):
                button = page.get_by_role("button", name=re.compile(r"^显示更早"))
                if not button.count():
                    break
                before = page.evaluate("""() => {
                  const container = document.querySelector('.ace-chat-transcript-scroll');
                  container.scrollTop = 0;
                  const row = container.querySelector('[data-chat-row="true"]');
                  return {id: row.dataset.chatItemId, top: row.getBoundingClientRect().top};
                }""")
                requests_before = page.evaluate("performance.getEntriesByType('resource').filter(e => e.name.includes('before=')).length")
                button.first.evaluate("element => element.click()")
                page.wait_for_timeout(1000)
                after = page.evaluate("""id => {
                  const row = document.querySelector(`[data-chat-item-id="${id}"]`);
                  return row ? {top: row.getBoundingClientRect().top} : null;
                }""", before["id"])
                requests_after = page.evaluate("performance.getEntriesByType('resource').filter(e => e.name.includes('before=')).length")
                result["paging_checks"].append({"anchor_retained": after is not None,
                    "scroll_delta_px": None if after is None else round(after["top"] - before["top"], 3),
                    "network_page": requests_after > requests_before})
                if requests_after > requests_before:
                    break
        browser.close()
        return result


def measure(args):
    result = {"executable_sha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
              "profile": str(args.profile), "mode": "warm filesystem cache; synthetic data"}
    with daemon(args.executable, args.profile, args.static_dir) as (base, token, manifest):
        if args.browser:
            result["browser"] = browser_measure(base, token, manifest, args.seconds, args.session_index, args.check_paging)
        else:
            workspace = manifest["project_hash"]
            listing = f"/api/workspaces/{workspace}/sessions"
            result["bounded_list"] = request(base, token, listing + "?limit=5")
            result["full_list"] = request(base, token, listing)
            result["global_list"] = request(base, token, "/api/sessions")
            result["sessions"] = []
            for entry in manifest["sessions"][:len(manifest["parameters"]["large_mib"])]:
                sid = entry["id"]
                history = f"/api/sessions/{sid}/messages?since=0&workspace={workspace}"
                sample = {"id": sid, "jsonl_bytes": entry["bytes"],
                          "resume": request(base, token, listing + f"/{sid}/resume", "POST")}
                sample["history"] = request(base, token, history)
                with ThreadPoolExecutor(max_workers=2) as executor:
                    pending = executor.submit(request, base, token, history)
                    time.sleep(.03)
                    sample["concurrent_list"] = request(base, token, listing + "?limit=5")
                    sample["concurrent_history"] = pending.result()
                sample["tail"] = request(base, token, history + "&limit=200")
                result["sessions"].append(sample)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--static-dir", type=Path)
    parser.add_argument("--browser", action="store_true")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--session-index", type=int, default=0)
    parser.add_argument("--check-paging", action="store_true")
    args = parser.parse_args()
    result = measure(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
