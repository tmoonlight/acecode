#!/usr/bin/env python3
"""Live check of the iOS Simulator integration against a booted simulator.

Builds acecode-ios-sim, the Seatbelt argv printer and the Desktop native view
smoke straight from the sources with clang++, boots a simulator and verifies
that input really takes effect (read back through the accessibility tree),
plus screenshots, streams, display info, the Seatbelt profile and the native
view. The ios-simulator-live workflow runs it on GitHub's xcode-27 runner to
cover the dtuhidd input transport (CoreSimulator 1155.4+); it also runs on any
Mac with Xcode and a simulator runtime.

usage: live_check.py --json-include DIR [--developer-dir DIR] [--device NAME]
                     [--out DIR] [--no-sandbox] [--hid indigo|dtuhid]
"""

import argparse
import json
import os
import re
import select
import struct
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HELPER_DIR = "src/adapters/tool/ios_simulator/helper"
NATIVE_SOURCES = [f"{HELPER_DIR}/sim_core.mm", f"{HELPER_DIR}/sim_guest.mm", f"{HELPER_DIR}/sim_framebuffer.mm"]
HELPER_SOURCES = [f"{HELPER_DIR}/helper_main.mm", f"{HELPER_DIR}/sim_output.mm", f"{HELPER_DIR}/sim_hid.mm",
                  f"{HELPER_DIR}/sim_display.mm", f"{HELPER_DIR}/sim_ax.mm"]
SANDBOX_ARGV_MAIN = r'''
#include "tool/ios_simulator/helper_sandbox.hpp"
#include <cstdio>
using namespace acecode::ios_simulator;
int main(int argc, char** argv) {
    if (argc < 3) return 64;
    std::vector<std::string> helper;
    for (int i = 3; i < argc; ++i) helper.push_back(argv[i]);
    for (const auto& arg : sandboxed_helper_argv(current_helper_sandbox_paths(argv[1], argv[2]), helper)) {
        std::fwrite(arg.data(), 1, arg.size(), stdout);
        std::fputc(0, stdout);
    }
    return 0;
}
'''
# US keyboard HID usages for lowercase letters.
LETTER_USAGE = {chr(ord("a") + i): 0x04 + i for i in range(26)}

results = []


def record(name, ok, detail=""):
    results.append({"check": name, "ok": bool(ok), "detail": detail})
    print(f"{'[ok]  ' if ok else '[FAIL]'} {name}{(': ' + detail) if detail else ''}", flush=True)
    return ok


def run(cmd, **kw):
    print("+ " + " ".join(cmd[:6]) + (" …" if len(cmd) > 6 else ""), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def build(out, json_include):
    common = ["xcrun", "clang++", "-std=c++17", "-O1", "-mmacosx-version-min=14.0", "-fobjc-arc",
              "-I", os.path.join(ROOT, "src/adapters"), "-I", json_include]
    sources = [os.path.join(ROOT, s) for s in NATIVE_SOURCES]
    helper = os.path.join(out, "acecode-ios-sim")
    run(common + sources + [os.path.join(ROOT, s) for s in HELPER_SOURCES] + [
        "-framework", "Foundation", "-framework", "CoreGraphics", "-framework", "Accelerate",
        "-framework", "ImageIO", "-framework", "IOSurface", "-o", helper])
    main_path = os.path.join(out, "sandbox_argv_main.cpp")
    with open(main_path, "w") as f:
        f.write(SANDBOX_ARGV_MAIN)
    argv_tool = os.path.join(out, "sandbox_argv")
    run(["xcrun", "clang++", "-std=c++17", "-I", os.path.join(ROOT, "src/adapters"), main_path,
         os.path.join(ROOT, "src/adapters/tool/ios_simulator/helper_sandbox.cpp"), "-o", argv_tool])
    smoke = os.path.join(out, "ios_simulator_view_mac_smoke")
    run(common + ["-I", os.path.join(ROOT, "src/apps"), os.path.join(ROOT, "tests/desktop/helpers/ios_simulator_view_mac_smoke.mm"),
                  os.path.join(ROOT, "src/apps/desktop/ios_simulator_view_mac.mm"),
                  os.path.join(ROOT, "src/apps/desktop/ios_simulator_view_layout.cpp")] + sources + [
        "-framework", "Foundation", "-framework", "AppKit", "-framework", "QuartzCore", "-framework", "IOSurface",
        "-o", smoke])
    return helper, argv_tool, smoke


def simctl(developer_dir, *args, timeout=300):
    return subprocess.run([os.path.join(developer_dir, "usr/bin/simctl"), *args], capture_output=True, text=True,
                          timeout=timeout)


def pick_device(developer_dir, wanted):
    listing = json.loads(simctl(developer_dir, "list", "-j", "devices", "available").stdout)
    candidates = []
    for runtime, devices in listing.get("devices", {}).items():
        match = re.search(r"iOS-(\d+)-(\d+)", runtime)
        if not match:
            continue
        version = (int(match.group(1)), int(match.group(2)))
        for device in devices:
            if device.get("name", "").startswith("iPhone"):
                candidates.append((device.get("name") == wanted, version, device["udid"], device["name"], runtime))
    if not candidates:
        raise SystemExit("no available iPhone simulator")
    candidates.sort(reverse=True)
    _, version, udid, name, runtime = candidates[0]
    return udid, name, runtime


class Helper:
    def __init__(self, argv, env):
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        self.buf = b""
        self.next_id = 0
        self.frames = 0

    def _read(self, deadline):
        while True:
            if len(self.buf) >= 8:
                body, json_len = struct.unpack("<II", self.buf[:8])
                if len(self.buf) >= 4 + body:
                    header = json.loads(self.buf[8:8 + json_len])
                    binary = self.buf[8 + json_len:4 + body]
                    self.buf = self.buf[4 + body:]
                    return header, binary
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError("helper reply timed out")
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(self.proc.stdout.fileno(), 1 << 16)
            if not chunk:
                raise RuntimeError(f"helper exited (code {self.proc.poll()})")
            self.buf += chunk

    def call(self, op, timeout=60, **kw):
        self.next_id += 1
        request = dict(id=self.next_id, op=op, **kw)
        data = json.dumps(request).encode()
        self.proc.stdin.write(struct.pack("<II", 4 + len(data), len(data)) + data)
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while True:
            header, binary = self._read(deadline)
            if header.get("id") == self.next_id:
                return header, binary
            if header.get("event") == "frame":
                self.frames += 1

    def drain(self, seconds):
        deadline = time.time() + seconds
        try:
            while True:
                header, _ = self._read(deadline)
                if header.get("event") == "frame":
                    self.frames += 1
        except TimeoutError:
            pass

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def walk(node):
    yield node
    for child in node.get("children", []) or []:
        yield from walk(child)


def find(tree, predicate):
    for node in walk(tree.get("root", {})):
        if predicate(node):
            return node
    return None


def center(node):
    frame = node.get("frame", {})
    return frame.get("x", 0) + frame.get("width", 0) / 2, frame.get("y", 0) + frame.get("height", 0) / 2


def ui_tree(helper, udid):
    header, _ = helper.call("ui_describe", udid=udid, mode="tree", max_depth=40, max_nodes=800, timeout=60)
    return header.get("result", {}) if header.get("ok") else {}


def tap(helper, udid, x, y):
    helper.call("touch", udid=udid, phase="down", x=x, y=y)
    time.sleep(0.08)
    helper.call("touch", udid=udid, phase="up", x=x, y=y, flush=True)


def drag(helper, udid, x1, y1, x2, y2, steps=12):
    events = [{"type": "touch", "phase": "down", "x": x1, "y": y1}]
    for step in range(1, steps + 1):
        t = step / steps
        events.append({"type": "delay", "ms": 16})
        events.append({"type": "touch", "phase": "down", "x": x1 + (x2 - x1) * t, "y": y1 + (y2 - y1) * t})
    events.append({"type": "delay", "ms": 16})
    events.append({"type": "touch", "phase": "up", "x": x2, "y": y2})
    return helper.call("touch_sequence", udid=udid, events=events)


def wait_for(predicate, seconds, interval=0.75):
    deadline = time.time() + seconds
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(interval)
    return predicate()


def label_of(node):
    return str(node.get("label") or node.get("identifier") or "")


def check_device(helper, udid, out, developer_dir):
    # First calls load Xcode's frameworks and connect to CoreSimulatorService;
    # hosted VMs can take most of a minute for that.
    hello, _ = helper.call("hello", timeout=180)
    info = hello.get("result", {})
    record("helper starts", hello.get("ok"), f"CoreSimulator {info.get('core_simulator_version')} "
           f"transport {info.get('default_hid_transport')}")
    describe, _ = helper.call("describe", udid=udid, timeout=180)
    size = describe.get("result", {})
    width, height = size.get("point_width", 0), size.get("point_height", 0)
    record("describe", describe.get("ok") and width > 0, f"{size.get('name')} {width}x{height} rotation {size.get('rotation')}")
    shot, png = helper.call("screenshot", udid=udid, format="png", scale=1.0)
    if png:
        with open(os.path.join(out, "home.png"), "wb") as f:
            f.write(png)
    record("screenshot", shot.get("ok") and len(png) > 1000, f"{len(png)} bytes")

    # Settings must start at its root list (a previous run may have left it
    # on search results).
    simctl(developer_dir, "terminate", udid, "com.apple.Preferences")
    helper.call("button", udid=udid, name="home")
    time.sleep(1.5)
    home = wait_for(lambda: ui_tree(helper, udid) or None, 20)
    springboard_pid = (home or {}).get("pid")
    record("accessibility tree", bool(home), f"pid {springboard_pid}, {home and home.get('node_count')} nodes")
    settings = find(home or {}, lambda n: label_of(n) in ("Settings", "设置"))
    if not record("Settings icon found", settings is not None):
        return width, height
    x, y = center(settings)
    frontmost_app = lambda: (lambda t: t if t.get("pid") and t.get("pid") != springboard_pid else None)(
        ui_tree(helper, udid))
    tap(helper, udid, x, y)
    opened = wait_for(frontmost_app, 20)
    if not opened:  # a cold runner can still be finishing SpringBoard's launch
        _, png = helper.call("screenshot", udid=udid, format="png", scale=0.5)
        with open(os.path.join(out, "settings_first_tap.png"), "wb") as f:
            f.write(png)
        tap(helper, udid, x, y)
        opened = wait_for(frontmost_app, 25)
    record("tap opens Settings (touch input)", bool(opened), f"frontmost pid {(opened or {}).get('pid')}")
    if opened:
        # Settings fills its list a moment after it becomes frontmost.
        general = lambda n: label_of(n) in ("General", "通用")
        before = wait_for(lambda: find(ui_tree(helper, udid), general), 25)
        if before:
            bx, by = center(before)
            drag(helper, udid, width / 2, height * 0.75, width / 2, height * 0.3)
            time.sleep(1.5)
            after = find(ui_tree(helper, udid), general)
            moved = after is None or center(after)[1] < by - 40
            record("drag scrolls the list (touch sequence)", moved,
                   f"General y {by:.0f} -> {center(after)[1]:.0f}" if after else "General scrolled away")
        else:
            record("drag scrolls the list (touch sequence)", False, "no General cell")
        # Back to the top and type into the search field (hardware keyboard).
        drag(helper, udid, width / 2, height * 0.3, width / 2, height * 0.85)
        time.sleep(1.0)
        drag(helper, udid, width / 2, height * 0.3, width / 2, height * 0.85)
        time.sleep(1.0)
        tree = ui_tree(helper, udid)
        # The search *field* (a "Search" settings row also exists); after
        # scrolling iOS folds it into the bottom toolbar.
        search = find(tree, lambda n: "field" in str(n.get("role", "")).lower()
                      and (str(n.get("placeholder", "")) in ("Search", "搜索") or label_of(n) in ("Search", "搜索")))
        if search is None:
            search = find(tree, lambda n: label_of(n) in ("Toolbar", "工具栏"))
        if search:
            # A tap while the list is still coasting only stops the scroll;
            # retry once when typing leaves the screen unchanged.
            for attempt in range(2):
                sx, sy = center(search)
                tap(helper, udid, sx, sy)
                time.sleep(1.5)
                _, before_png = helper.call("screenshot", udid=udid, format="png", scale=0.5)
                events = []
                for ch in "wifi":
                    events += [{"usage": LETTER_USAGE[ch], "phase": "down"},
                               {"usage": LETTER_USAGE[ch], "phase": "up"}, {"delay_ms": 40}]
                keys, _ = helper.call("keys", udid=udid, events=events)
                time.sleep(1.5)
                _, after_png = helper.call("screenshot", udid=udid, format="png", scale=0.5)
                changed = bool(before_png) and before_png != after_png
                if changed:
                    break
                search = find(ui_tree(helper, udid), lambda n: "field" in str(n.get("role", "")).lower()) or search
            for name, data in (("before_typing.png", before_png), ("after_typing.png", after_png)):
                with open(os.path.join(out, name), "wb") as f:
                    f.write(data)
            typed = find(ui_tree(helper, udid), lambda n: "wifi" in str(n.get("value", "")).lower())
            # An input method (e.g. Pinyin) keeps the text as marked text with
            # an empty accessibility value; the screen still shows the letters.
            record("keys type into the search field (keyboard input)", keys.get("ok") and (typed is not None or changed),
                   f"value {typed.get('value')!r}" if typed else
                   ("screen changed (after_typing.png), no accessibility value" if changed else "no change"))
        else:
            with open(os.path.join(out, "settings_tree.json"), "w") as f:
                json.dump(tree, f, ensure_ascii=False, indent=1)
            record("keys type into the search field (keyboard input)", False, "no search field (settings_tree.json)")
        helper.call("button", udid=udid, name="home")
        back = wait_for(lambda: (lambda t: t if t.get("pid") == springboard_pid else None)(ui_tree(helper, udid)), 10)
        record("home button returns to SpringBoard (button input)", bool(back))
    stream, _ = helper.call("start_stream", udid=udid, stream=1, fps=15, scale=0.4, quality=0.6)
    helper.drain(1.0)
    initial = helper.frames
    # Swipe between home-screen pages so the screen keeps changing.
    drag(helper, udid, width * 0.85, height * 0.6, width * 0.15, height * 0.6)
    helper.drain(1.5)
    drag(helper, udid, width * 0.15, height * 0.6, width * 0.85, height * 0.6)
    helper.drain(1.5)
    helper.call("stop_stream", udid=udid, stream=1)
    print(f"stream frames: initial {initial}, total {helper.frames}", flush=True)
    record("JPEG stream delivers frames", stream.get("ok") and helper.frames > 0,
           f"{helper.frames} frames, start {json.dumps(stream)[:160]}")
    # Xcode 27 runtimes answer the CoreDevice display-info request; older ones
    # report it unsupported and fall back to Simulator.app's window setting.
    # display_info also asks over a fresh connection (slow while the guest is
    # busy); "cached" is what the helper itself has been using all along.
    parsed = False
    for attempt in range(3):
        display, _ = helper.call("display_info", udid=udid)
        info = display.get("result", {})
        with open(os.path.join(out, f"display_info_{attempt}.json"), "w") as f:
            json.dump(info, f, indent=1)
        cached = info.get("cached") or {}
        parsed = info.get("unsupported") or info.get("rotation") in (0, 90, 180, 270) or cached.get("answered")
        if parsed:
            break
    record("display info", display.get("ok") and parsed,
           f"rotation {info.get('rotation')} unsupported {info.get('unsupported')} "
           f"window {info.get('window_rotation')} cached {json.dumps(cached)}")
    return width, height


def check_sandbox_denials(argv_tool, helper_path, developer_dir):
    argv = subprocess.run([argv_tool, helper_path, developer_dir, "/bin/true"], capture_output=True).stdout
    parts = [a.decode() for a in argv.split(b"\0")[:-1]][:-1]  # drop the placeholder command
    secret = os.path.expanduser("~/acecode-live-check-secret.txt")
    with open(secret, "w") as f:
        f.write("secret\n")
    read = subprocess.run(parts + ["/bin/cat", secret], capture_output=True)
    record("sandbox refuses reading home files", read.returncode != 0)
    net = subprocess.run(parts + ["/usr/bin/curl", "-sS", "-m", "8", "https://example.com", "-o", "/dev/null"],
                         capture_output=True)
    record("sandbox refuses network", net.returncode != 0)
    probe = os.path.expanduser("~/acecode-live-check-write")
    subprocess.run(parts + ["/usr/bin/touch", probe], capture_output=True)
    record("sandbox refuses writing home", not os.path.exists(probe))
    os.remove(secret)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--json-include", required=True)
    parser.add_argument("--developer-dir", default="")
    parser.add_argument("--device", default="iPhone 17e")
    parser.add_argument("--out", default=os.path.join(ROOT, "build", "ios-sim-live"))
    parser.add_argument("--no-sandbox", action="store_true")
    parser.add_argument("--hid", default="")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    developer_dir = args.developer_dir or subprocess.run(["xcode-select", "-p"], capture_output=True,
                                                         text=True).stdout.strip()
    print(subprocess.run([os.path.join(developer_dir, "usr/bin/xcodebuild"), "-version"], capture_output=True,
                         text=True).stdout, flush=True)
    helper_path, argv_tool, smoke = build(args.out, args.json_include)

    udid, name, runtime = pick_device(developer_dir, args.device)
    print(f"device {name} {udid} {runtime}", flush=True)
    simctl(developer_dir, "boot", udid)
    boot = simctl(developer_dir, "bootstatus", udid, "-b", timeout=900)
    record("simulator boots", boot.returncode == 0, runtime)
    time.sleep(20)  # let SpringBoard settle after the first boot

    helper_argv = [helper_path, "--developer-dir", developer_dir] + (["--hid", args.hid] if args.hid else [])
    if not args.no_sandbox:
        raw = subprocess.run([argv_tool, helper_path, developer_dir] + helper_argv, capture_output=True).stdout
        helper_argv = [a.decode() for a in raw.split(b"\0")[:-1]]
    helper = Helper(helper_argv, dict(os.environ))
    try:
        check_device(helper, udid, args.out, developer_dir)
    except Exception as error:  # report and keep the summary
        record("helper session", False, repr(error))
    finally:
        helper.close()
    if not args.no_sandbox:
        check_sandbox_denials(argv_tool, helper_path, developer_dir)
    view = subprocess.run([smoke, udid, developer_dir], capture_output=True, text=True, timeout=180)
    print(view.stdout, flush=True)
    record("Desktop native view smoke", "SMOKE_OK" in view.stdout, view.stderr.strip()[-300:])

    simctl(developer_dir, "shutdown", udid)
    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump({"device": name, "runtime": runtime, "sandbox": not args.no_sandbox, "results": results}, f,
                  indent=2)
    failed = [r for r in results if not r["ok"]]
    print(f"{len(results) - len(failed)}/{len(results)} checks passed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
