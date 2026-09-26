#!/usr/bin/env python3
"""Run the P0-12 probe in four disposable, isolated Windows console processes."""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import socketserver
import subprocess
import sys
import threading
import time


SESSION_ID = "20260927-000000-0012"
SCENARIOS = ("ordinary", "resume", "copilot-unauthenticated", "mcp-configured")


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8", newline="\n")


def cwd_hash(path):
    # Same FNV-1a and ASCII normalization as src/utils/cwd_hash.cpp. The
    # scratch directory is real (no symlinks/subst) and is checked by resume.
    normalized = str(path.resolve()).replace("\\", "/").lower().rstrip("/")
    result = 14695981039346656037
    for byte in normalized.encode("utf-8"):
        result = ((result ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{result:016x}"


def launch_console(executable, arguments, cwd, environment, timeout_seconds=40):
    """New hidden console with its own standard handles (no inherited pipes)."""
    class StartupInfo(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
            ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR),
            ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
            ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
            ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
            ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
            ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
            ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
            ("hStdInput", wintypes.HANDLE), ("hStdOutput", wintypes.HANDLE),
            ("hStdError", wintypes.HANDLE),
        ]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                    ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateProcessW.argtypes = [
        wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
        wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
        ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInfo),
    ]
    kernel.CreateProcessW.restype = wintypes.BOOL
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetExitCodeProcess.restype = wintypes.BOOL
    kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    startup, process = StartupInfo(), ProcessInfo()
    startup.cb = ctypes.sizeof(startup)
    startup.dwFlags = 1  # STARTF_USESHOWWINDOW, without USESTDHANDLES.
    startup.wShowWindow = 0  # SW_HIDE; this is a probe, not an interactive app.
    command = ctypes.create_unicode_buffer(subprocess.list2cmdline(
        [str(executable), *arguments]))
    env_block = ctypes.create_unicode_buffer("\0".join(
        f"{key}={value}" for key, value in sorted(environment.items(),
                                                key=lambda pair: pair[0].upper())
    ) + "\0\0")
    if not kernel.CreateProcessW(str(executable), command, None, None, False,
                                 0x10 | 0x400, env_block, str(cwd),
                                 ctypes.byref(startup), ctypes.byref(process)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        waited = kernel.WaitForSingleObject(process.hProcess, timeout_seconds * 1000)
        if waited == 0x102:
            kernel.TerminateProcess(process.hProcess, 93)
            kernel.WaitForSingleObject(process.hProcess, 5000)
            raise RuntimeError(f"Probe timed out (owned process {process.dwProcessId})")
        if waited != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        result = wintypes.DWORD()
        if not kernel.GetExitCodeProcess(process.hProcess, ctypes.byref(result)):
            raise ctypes.WinError(ctypes.get_last_error())
        return result.value
    finally:
        kernel.CloseHandle(process.hThread)
        kernel.CloseHandle(process.hProcess)


def fixture(case, root, proxy_port):
    # Keep cwd below the fixture profile so instruction/skill discovery stops
    # at that profile instead of walking through the developer's real home.
    profile = root / case / "user"
    working = profile / "workspace"
    working.mkdir(parents=True, exist_ok=False)
    data = profile / ".acecode"
    data.mkdir()
    copilot = case == "copilot-unauthenticated"
    config = {
        "default_model_name": "p012-fixture",
        "saved_models": [{
            "name": "p012-fixture", "provider": "copilot" if copilot else "openai",
            "model": "gpt-4o" if copilot else "p012-fixture",
            "base_url": "" if copilot else f"http://127.0.0.1:{proxy_port}/v1",
            "api_key": "", "context_window": 8192,
        }],
        "models_dev": {"allow_network": False, "refresh_on_command_only": True},
        "network": {"proxy_mode": "manual", "proxy_probe_enabled": False,
                    "proxy_url": f"http://127.0.0.1:{proxy_port}"},
        "upgrade": {"base_url": f"http://127.0.0.1:{proxy_port}/aupdate/",
                    "timeout_ms": 30000},
        "skills": {"reuse_opencode": False, "external_dirs": []},
        "memory": {"enabled": False}, "lsp": {"enabled": False},
        "web_search": {"enabled": False}, "git": {"enabled": False},
        "features": {"hooks": False}, "computer_use": {"enabled": False},
        "desktop": {"notifications": {"enabled": False}},
        "tui": {"alt_screen_mode": "never"}, "ui": {"locale": "en-US"},
    }
    if case == "mcp-configured":
        config["mcp_servers"] = {
            "p012-delayed": {"command": sys.executable,
                             "args": [str(Path(__file__).resolve()), "--mcp-stall"],
                             "timeout_seconds": 30}
        }
    write_json(data / "config.json", config)
    if case == "resume":
        project = data / "projects" / cwd_hash(working)
        messages = [
            {"role": "user", "content": "启动快照：恢复这一轮。"},
            {"role": "assistant", "content": "已保存的回答。"},
        ]
        project.mkdir(parents=True)
        (project / (SESSION_ID + ".jsonl")).write_text(
            "".join(json.dumps(message, ensure_ascii=False) + "\n" for message in messages),
            encoding="utf-8", newline="\n")
        write_json(project / (SESSION_ID + ".meta.json"), {
            "id": SESSION_ID, "cwd": str(working),
            "created_at": "2026-09-27T00:00:00Z", "updated_at": "2026-09-27T00:00:00Z",
            "message_count": 2, "summary": "P0-12 fixture",
            "provider": "openai", "model": "p012-fixture",
            "model_preset": "p012-fixture", "title": "P0-12 fixture", "title_source": "user",
            "permission_mode": "default", "turn_count": 1,
        })
    return profile, working


def main():
    if sys.argv[1:] == ["--mcp-stall"]:
        # Receive initialize, but deliberately keep it pending. EOF ends this
        # owned helper when the probe closes its stdio pipe at the checkpoint.
        for _ in sys.stdin.buffer:
            pass
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--scratch", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-metadata", required=True, type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("This capture host uses Windows native console processes")
    executable, scratch, output = (p.resolve() for p in
                                    (args.executable, args.scratch, args.output))
    if any((output / (case + ".json")).exists() for case in SCENARIOS):
        raise SystemExit("Output snapshots already exist; use a fresh output directory")
    output.mkdir(parents=True, exist_ok=True)
    stop = threading.Event()

    class PendingProxy(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.settimeout(2)
            try:
                self.request.recv(8192)
                stop.wait(45)
            except OSError:
                pass

    class ProxyServer(socketserver.ThreadingTCPServer):
        daemon_threads = True

    with ProxyServer(("127.0.0.1", 0), PendingProxy) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            captures = []
            for case in SCENARIOS:
                profile, working = fixture(case, scratch, server.server_address[1])
                environment = {key: value for key, value in os.environ.items()
                               if not key.upper().startswith("ACECODE_")}
                # Child-only fixture profile; the host shell/user is unchanged.
                environment["USERPROFILE"] = str(profile)
                for name, relative in (("APPDATA", "roaming"),
                                       ("LOCALAPPDATA", "local"),
                                       ("TEMP", "temp"), ("TMP", "temp"),
                                       ("TMPDIR", "temp")):
                    directory = profile / relative
                    directory.mkdir(exist_ok=True)
                    environment[name] = str(directory)
                environment["ACECODE_P012_SCENARIO"] = case
                environment["ACECODE_P012_SNAPSHOT_PATH"] = str(output / (case + ".json"))
                arguments = ["--resume", SESSION_ID] if case == "resume" else []
                started = time.monotonic()
                exit_code = launch_console(executable, arguments, working, environment)
                if exit_code != 0:
                    raise RuntimeError(f"{case}: probe exited {exit_code}; inspect {profile}")
                path = output / (case + ".json")
                snapshot = json.loads(path.read_text("utf-8"))
                if snapshot["scenario"] != case:
                    raise RuntimeError("Wrong scenario recorded by runtime probe")
                captures.append({"scenario": case, "exit_code": exit_code,
                                 "elapsed_seconds": round(time.monotonic() - started, 3),
                                 "snapshot_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                 "total_messages": snapshot["total_messages"]})
                print(f"{case}: {snapshot['total_messages']} messages, exit {exit_code}", flush=True)
            write_json(output / "capture-manifest.json", {
                "source": json.loads(args.source_metadata.read_text("utf-8")),
                "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                "platform": sys.platform, "captures": captures,
                "observed_only_until_checkpoint": True,
                "input_network_state": "local proxy and MCP initialize kept pending",
            })
        finally:
            stop.set()
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
