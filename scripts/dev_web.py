#!/usr/bin/env python3
"""Start the ACECode Web UI daemon without starting the Desktop GUI."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
import webbrowser
from pathlib import Path

from dev_build_artifacts import find_named_artifacts


def find_project_root() -> Path:
    current = Path(__file__).resolve().parent
    for _ in range(6):
        if (current / "CMakeLists.txt").is_file() and (current / "web").is_dir():
            return current
        current = current.parent
    return Path(__file__).resolve().parent.parent


def find_executable(build_dir: Path) -> Path | None:
    name = "acecode.exe" if os.name == "nt" else "acecode"
    matches = find_named_artifacts(build_dir, [name])
    return matches[0] if matches else None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Start ACECode Web UI daemon without starting the Desktop GUI"
    )
    parser.add_argument("--build-dir", default="build", help="ACECode build directory")
    parser.add_argument("--cwd", default=None, help="Workspace directory served by the daemon")
    parser.add_argument("--port", type=int, default=None, help="Override the Web UI port")
    parser.add_argument(
        "--static-dir",
        default=None,
        help="Web assets directory; defaults to <project>/web/dist",
    )
    parser.add_argument(
        "--run-dir", default=None, help="Isolate daemon runtime files to this directory"
    )
    parser.add_argument(
        "--foreground",
        action="store_true",
        help="Keep the daemon attached to this console for debugging",
    )
    parser.add_argument(
        "--no-browser", action="store_true", help="Do not open the Web UI browser"
    )
    args, extra = parser.parse_known_args()

    project_root = find_project_root()
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = project_root / build_dir
    build_dir = build_dir.resolve()

    executable = find_executable(build_dir)
    if executable is None:
        print(f"[ERROR] ACECode executable not found under: {build_dir}", file=sys.stderr)
        print("        Build the acecode target first, or pass --build-dir.", file=sys.stderr)
        return 1

    static_dir = Path(args.static_dir) if args.static_dir else project_root / "web" / "dist"
    if not static_dir.is_absolute():
        static_dir = project_root / static_dir
    static_dir = static_dir.resolve()
    if not (static_dir / "index.html").is_file():
        print(f"[ERROR] Web UI assets not found: {static_dir / 'index.html'}", file=sys.stderr)
        print("        Run `pnpm --dir web build` first, or pass --static-dir.", file=sys.stderr)
        return 1

    workspace = Path(args.cwd).resolve() if args.cwd else project_root
    if not workspace.is_dir():
        print(f"[ERROR] Workspace directory not found: {workspace}", file=sys.stderr)
        return 1

    command = [str(executable), "daemon"]
    if args.foreground:
        command.append("--foreground")
    if args.port is not None:
        if not 1 <= args.port <= 65535:
            print("[ERROR] --port must be between 1 and 65535", file=sys.stderr)
            return 1
        command.append(f"--port={args.port}")
    command.extend((f"--cwd={workspace}", f"--static-dir={static_dir}"))
    if args.run_dir:
        run_dir = Path(args.run_dir)
        if not run_dir.is_absolute():
            run_dir = project_root / run_dir
        command.append(f"--run-dir={run_dir.resolve()}")
    command.extend(extra)

    print(f"[INFO] Starting Web UI daemon: {executable}")
    print(f"[INFO] Workspace: {workspace}")
    print(f"[INFO] Static assets: {static_dir}")
    print("[INFO] Desktop GUI is not started.", flush=True)

    if args.foreground:
        return subprocess.run(command, cwd=project_root).returncode

    runtime_dir = _runtime_dir(project_root, args.run_dir)
    previous_port_mtime_ns = _port_mtime_ns(runtime_dir)
    run_options = {"cwd": project_root}
    if os.name == "nt":
        run_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    result = subprocess.run(command, **run_options)

    # The Windows daemon wrapper can time out before its worker has finished
    # loading configuration. Accept a nonzero wrapper exit only when it is
    # followed by a freshly written port file from this launch.
    allow_existing_port = result.returncode in (0, 6)
    port = _wait_for_port(runtime_dir, previous_port_mtime_ns, allow_existing_port)
    if port is None:
        print("[ERROR] Daemon started without a fresh readable Web UI port.", file=sys.stderr)
        return result.returncode or 1

    _open_web_ui(port, args.no_browser, already_running=result.returncode == 6)
    return 0


def _runtime_dir(project_root: Path, configured: str | None) -> Path:
    if configured:
        path = Path(configured)
        return path if path.is_absolute() else (project_root / path).resolve()
    return Path.home() / ".acecode" / "run"


def _open_web_ui(port: int, no_browser: bool, already_running: bool = False) -> None:
    url = f"http://127.0.0.1:{port}/"
    prefix = "already running; " if already_running else ""
    print(f"[INFO] Web UI {prefix}{url}", flush=True)
    if not no_browser:
        print("[INFO] Opening browser...", flush=True)
        if os.name == "nt":
            os.startfile(url)
        else:
            webbrowser.open(url)


def _port_mtime_ns(runtime_dir: Path) -> int | None:
    try:
        return (runtime_dir / "daemon.port").stat().st_mtime_ns
    except OSError:
        return None


def _wait_for_port(runtime_dir: Path, previous_mtime_ns: int | None = None, allow_existing: bool = True) -> int | None:
    port_file = runtime_dir / "daemon.port"
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            port = int(port_file.read_text(encoding="utf-8").strip())
            mtime_ns = port_file.stat().st_mtime_ns
            if 1 <= port <= 65535 and (allow_existing or previous_mtime_ns is None or mtime_ns > previous_mtime_ns):
                return port
        except (OSError, ValueError):
            pass
        time.sleep(0.1)
    return None


if __name__ == "__main__":
    raise SystemExit(main())
