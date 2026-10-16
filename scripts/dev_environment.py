#!/usr/bin/env python3
"""Launch ACECode Web, Desktop, or TUI development environments safely."""

from __future__ import annotations

import argparse
import errno
import importlib.util
import json
import os
import platform
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from dev_build_artifacts import find_named_artifacts

LAUNCH_TARGETS = ("web", "desktop", "tui")
PRUNE_TARGET = "prune"
TARGETS = (*LAUNCH_TARGETS, PRUNE_TARGET)
CACHE_MARKER = ".acecode-sccache.json"
WORKER_LOG_NAME = "daemon_worker.log"
WORKER_LOG_MAX_BYTES = 1024 * 1024
PORT_RANGE_START = 28080
PORT_RANGE_COUNT = 201
PORT_RANGE_END = PORT_RANGE_START + PORT_RANGE_COUNT - 1
DAEMON_STARTUP_LOG_NAME = "daemon_startup.log"


@dataclass(frozen=True)
class BuildCandidate:
    build_dir: Path
    source_dir: Path
    executable: Path
    configuration: str | None = None


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def native_executable_name(name: str) -> str:
    return f"{name}.exe" if os.name == "nt" else name


def executable_for(build_dir: Path, target: str) -> Path | None:
    name = native_executable_name("acecode-desktop" if target == "desktop" else "acecode")
    app_bundle = "ACECode.app" if target == "desktop" and sys.platform == "darwin" else None
    matches = find_named_artifacts(build_dir, [name], app_bundle)
    for executable in matches:
        # A nested preset is a different CMake build, even when its binaries
        # happen to be below this build's directory. Validate it separately.
        directory = executable.parent
        while directory != build_dir and not (directory / "CMakeCache.txt").is_file():
            directory = directory.parent
        if directory == build_dir:
            return executable
    return None


def artifact_configuration(build_dir: Path, executable: Path) -> str | None:
    configurations = cmake_cache_value(build_dir / "CMakeCache.txt", "CMAKE_CONFIGURATION_TYPES")
    if not configurations:
        return None
    relative_parts = executable.relative_to(build_dir).parts[:-1]
    for configuration in configurations.split(";"):
        if configuration in relative_parts:
            return configuration
    # Shared output directories still require an explicit build configuration.
    return "Release" if "Release" in configurations.split(";") else configurations.split(";")[0]


def cmake_cache_value(cache: Path, key: str) -> str | None:
    try:
        pattern = re.compile(rf"^{re.escape(key)}(?::[^=]+)?=(.*)$")
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            match = pattern.match(line)
            if match:
                return match.group(1)
    except OSError:
        return None
    return None


def cmake_source_dir(build_dir: Path) -> Path | None:
    value = cmake_cache_value(build_dir / "CMakeCache.txt", "CMAKE_HOME_DIRECTORY")
    return Path(value).resolve() if value else None


def configured_for_desktop(build_dir: Path) -> bool:
    return cmake_cache_value(build_dir / "CMakeCache.txt", "ACECODE_BUILD_DESKTOP") == "ON"


def current_commit(root: Path) -> str | None:
    result = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        text=True,
        capture_output=True,
        check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else None


def registered_worktrees(root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(root), "worktree", "list", "--porcelain"],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return []
    return [Path(line[len("worktree "):]).resolve() for line in result.stdout.splitlines() if line.startswith("worktree ")]


def build_directories(worktree: Path, explicit: Path | None = None) -> Iterable[Path]:
    if explicit:
        yield explicit.resolve()
        return
    build_root = worktree / "build"
    if build_root.is_dir():
        yield build_root
        yield from (path for path in build_root.iterdir() if path.is_dir())


def platform_matches(build_dir: Path) -> bool:
    triplet = cmake_cache_value(build_dir / "CMakeCache.txt", "VCPKG_TARGET_TRIPLET")
    if not triplet:
        return True
    system = platform.system().lower()
    expected_systems = ("windows", "mingw") if system == "windows" else ("osx",) if system == "darwin" else ("linux",)
    machine = native_machine()
    expected_arch = "arm64" if machine in {"arm64", "aarch64"} else "x64" if machine in {"amd64", "x86_64"} else None
    triplet_lower = triplet.lower()
    return any(expected in triplet_lower for expected in expected_systems) and (expected_arch is None or expected_arch in triplet_lower)


def find_compatible_build(root: Path, target: str, explicit: Path | None = None) -> BuildCandidate | None:
    root = root.resolve()
    directories = [(root / explicit).resolve()] if explicit else sorted(
        build_directories(root),
        key=lambda directory: (configured_for_desktop(directory), str(directory).lower()),
    )
    for build_dir in directories:
        source_dir = cmake_source_dir(build_dir)
        executable = executable_for(build_dir, target)
        if source_dir != root or not executable or not platform_matches(build_dir):
            continue
        if target == "desktop" and not configured_for_desktop(build_dir):
            continue
        return BuildCandidate(build_dir.resolve(), source_dir, executable, artifact_configuration(build_dir, executable))
    return None


def default_preset(target: str) -> str | None:
    system = platform.system().lower()
    machine = native_machine()
    arch = "arm64" if machine in {"arm64", "aarch64"} else "x64" if machine in {"amd64", "x86_64"} else None
    if not arch:
        return None
    prefix = {"windows": "windows", "darwin": "macos", "linux": "linux"}.get(system)
    if not prefix:
        return None
    suffix = "-desktop-release" if target == "desktop" else "-release"
    return f"{prefix}-{arch}{suffix}"


def native_machine() -> str:
    if platform.system().lower() == "windows":
        architecture = os.environ.get("PROCESSOR_ARCHITEW6432") or os.environ.get("PROCESSOR_ARCHITECTURE")
        if architecture:
            return architecture.lower()
    return platform.machine().lower()


def needs_msvc(candidate: BuildCandidate | None) -> bool:
    if candidate is None:
        return True  # The Windows presets use MSVC.
    cache = candidate.build_dir / "CMakeCache.txt"
    triplet = (cmake_cache_value(cache, "VCPKG_TARGET_TRIPLET") or "").lower()
    compiler = (cmake_cache_value(cache, "CMAKE_CXX_COMPILER") or "").lower()
    return "mingw" not in triplet and not compiler.endswith(("g++.exe", "g++"))


def ensure_windows_environment(root: Path, candidate: BuildCandidate | None) -> bool:
    if os.name != "nt" or not needs_msvc(candidate):
        return True
    environment = os.environ.copy()
    environment["ACECODE_DEV_ENV_SCRIPT"] = str(root / "scripts/dev_windows_env.bat")
    try:
        # Expanding the path once, inside quotes, preserves spaces and cmd
        # metacharacters in the checkout path. Never log the environment dump.
        result = subprocess.run('"%ACECODE_DEV_ENV_SCRIPT%" --print-env', shell=True,
                                env=environment, capture_output=True, text=True,
                                errors="replace", timeout=60, check=False,
                                creationflags=subprocess.CREATE_NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"[ERROR] Could not initialize the Visual Studio environment: {error}", file=sys.stderr)
        return False
    if result.returncode != 0:
        print(result.stdout.strip() or result.stderr.strip() or "[ERROR] Visual Studio C++ initialization failed.", file=sys.stderr)
        return False
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator and key and not key.startswith("="):
            os.environ[key] = value
    return True


def ask_to_build(preset: str, target: str, assume_yes: bool) -> bool:
    binary_dir = f"build/{preset}"
    executable_target = "acecode-desktop" if target == "desktop" else "acecode"
    print("[INFO] No compatible build was found in this repository's registered worktrees.")
    print(f"[INFO] Proposed configure preset: {preset} (BUILD_TESTING=OFF)")
    print(f"[INFO] Proposed build: cmake --build {binary_dir} --target {executable_target}")
    if assume_yes:
        return True
    if sys.stdin is None or not sys.stdin.isatty():
        print("[ERROR] Refusing to compile without interactive confirmation. Re-run with --yes to confirm.", file=sys.stderr)
        return False
    try:
        return input("Configure and build now? [y/N] ").strip().lower() in {"y", "yes"}
    except EOFError:
        print("[ERROR] Confirmation input was unavailable; no configuration or build was started.", file=sys.stderr)
        return False


def sccache_candidates() -> list[Path]:
    executable = "sccache.exe" if os.name == "nt" else "sccache"
    candidates: list[Path] = []
    from_path = shutil.which("sccache")
    if from_path:
        candidates.append(Path(from_path))
    if os.name == "nt":
        local_app_data = os.environ.get("LOCALAPPDATA")
        if local_app_data:
            candidates.extend([
                Path(local_app_data) / "Microsoft" / "WinGet" / "Links" / executable,
                Path(local_app_data) / "scoop" / "shims" / executable,
            ])
        chocolatey = os.environ.get("ChocolateyInstall", r"C:\\ProgramData\\chocolatey")
        candidates.append(Path(chocolatey) / "bin" / executable)
    elif sys.platform == "darwin":
        candidates.extend([Path("/opt/homebrew/bin/sccache"), Path("/usr/local/bin/sccache")])
    else:
        candidates.append(Path.home() / ".cargo" / "bin" / "sccache")
    return candidates


def find_sccache() -> Path | None:
    for candidate in sccache_candidates():
        if not candidate.is_file():
            continue
        resolved = candidate.resolve()
        try:
            result = subprocess.run(
                [str(resolved), "--version"],
                text=True,
                capture_output=True,
                check=False,
                timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        if result.returncode == 0:
            return resolved
    return None


def sccache_install_hint() -> str:
    if os.name == "nt":
        return "Install sccache with: winget install Mozilla.sccache"
    if sys.platform == "darwin":
        return "Install sccache with: brew install sccache"
    return "Install sccache with your package manager or cargo install sccache"


def cache_marker(build_dir: Path) -> Path:
    return build_dir / CACHE_MARKER


def cached_sccache_state(build_dir: Path) -> tuple[str | None, bool] | None:
    try:
        data = json.loads(cache_marker(build_dir).read_text(encoding="utf-8"))
        path = data.get("sccache")
        enabled = data.get("enabled", True)
        return (path if isinstance(path, str) else None, bool(enabled))
    except (OSError, ValueError, TypeError):
        return None


def write_sccache_marker(build_dir: Path, sccache: Path | None, enabled: bool = True) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    cache_marker(build_dir).write_text(json.dumps({"sccache": str(sccache) if sccache else None, "enabled": enabled}) + "\n", encoding="utf-8")


def cache_state_changed(build_dir: Path, sccache: Path | None) -> bool:
    state = cached_sccache_state(build_dir)
    return state is None or state[0] != (str(sccache) if sccache else None)


def sccache_disabled_for_build(build_dir: Path, sccache: Path | None) -> bool:
    state = cached_sccache_state(build_dir)
    return sccache is not None and state == (str(sccache), False)


def configure_build(root: Path, preset: str, build_dir: Path, sccache: Path | None) -> bool:
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        command = [
            "cmake", "-S", str(root), "-B", str(build_dir),
        ]
        triplet = cmake_cache_value(cache, "VCPKG_TARGET_TRIPLET")
        toolchain = cmake_cache_value(cache, "CMAKE_TOOLCHAIN_FILE")
        overlay = cmake_cache_value(cache, "VCPKG_OVERLAY_PORTS")
        if triplet:
            command.append(f"-DVCPKG_TARGET_TRIPLET={triplet}")
        if toolchain:
            command.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")
        if overlay:
            command.append(f"-DVCPKG_OVERLAY_PORTS={overlay}")
        if configured_for_desktop(build_dir):
            command.append("-DACECODE_BUILD_DESKTOP=ON")
    else:
        command = ["cmake", "--preset", preset, "-DBUILD_TESTING=OFF"]
    if sccache:
        command.extend([
            f"-DCMAKE_C_COMPILER_LAUNCHER={sccache}",
            f"-DCMAKE_CXX_COMPILER_LAUNCHER={sccache}",
        ])
    else:
        command.extend(["-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER="])
    if subprocess.run(command, cwd=root, check=False).returncode != 0:
        return False
    write_sccache_marker(build_dir, sccache)
    return True


def _sum_integer_leaves(value) -> int:
    if isinstance(value, bool):
        return 0
    if isinstance(value, int):
        return value
    if isinstance(value, dict):
        return sum(_sum_integer_leaves(child) for child in value.values())
    if isinstance(value, list):
        return sum(_sum_integer_leaves(child) for child in value)
    return 0


def sccache_stats(sccache: Path) -> dict[str, int] | None:
    result = subprocess.run([str(sccache), "--show-stats", "--stats-format=json"], text=True, capture_output=True, check=False)
    if result.returncode != 0:
        return None
    try:
        data = json.loads(result.stdout)
        stats = data["stats"]
    except (ValueError, KeyError, TypeError):
        return None
    return {
        "hits": _sum_integer_leaves(stats.get("cache_hits", {})),
        "misses": _sum_integer_leaves(stats.get("cache_misses", {})),
        "errors": sum(
            _sum_integer_leaves(stats.get(key, 0))
            for key in ("cache_errors", "cache_read_errors", "cache_write_errors", "dist_errors")
        ),
    }


def report_sccache_delta(before: dict[str, int] | None, after: dict[str, int] | None) -> None:
    if before is None or after is None:
        print("[INFO] sccache statistics unavailable; continuing.")
        return
    delta = {key: max(0, after[key] - before[key]) for key in before}
    print(f"[INFO] sccache this build: hits={delta['hits']} misses={delta['misses']} errors={delta['errors']}")


def build_target(root: Path, build_dir: Path, target: str, sccache: Path | None = None, configuration: str | None = None) -> bool:
    executable_target = "acecode-desktop" if target == "desktop" else "acecode"
    before = sccache_stats(sccache) if sccache else None
    command = ["cmake", "--build", str(build_dir), "--target", executable_target]
    if configuration:
        command.extend(("--config", configuration))
    result = subprocess.run(
        command,
        cwd=root,
        check=False,
    )
    if sccache:
        report_sccache_delta(before, sccache_stats(sccache))
    return result.returncode == 0


def configure_and_build(root: Path, preset: str, target: str, sccache: Path | None = None) -> Path | None:
    build_dir = root / "build" / preset
    if not configure_build(root, preset, build_dir, sccache):
        return None
    return build_dir if build_target(root, build_dir, target, sccache) else None


def web_worktree_is_clean(root: Path) -> bool:
    result = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--", "web"], text=True, capture_output=True, check=False)
    return result.returncode == 0 and not result.stdout.strip()


def newest_web_seed(root: Path) -> Path | None:
    commit = current_commit(root)
    if not commit or not web_worktree_is_clean(root):
        return None
    candidates: list[Path] = []
    for worktree in registered_worktrees(root):
        if worktree == root.resolve() or current_commit(worktree) != commit or not web_worktree_is_clean(worktree):
            continue
        index = worktree / "web" / "dist" / "index.html"
        if index.is_file():
            candidates.append(index)
    return max(candidates, key=lambda item: item.stat().st_mtime, default=None)


def load_web_builder(root: Path):
    script_path = root / "scripts" / "dev_desktop.py"
    spec = importlib.util.spec_from_file_location("dev_desktop_for_environment", script_path)
    if spec is None or spec.loader is None:
        return None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def seed_web_assets(root: Path, builder) -> bool:
    web_dir = root / "web"
    index = web_dir / "dist" / "index.html"
    if not builder.web_build_is_stale(web_dir, index):
        return False
    seed_index = newest_web_seed(root)
    if seed_index is None:
        return False
    try:
        if (web_dir / "dist").exists():
            shutil.rmtree(web_dir / "dist")
        shutil.copytree(seed_index.parent, web_dir / "dist")
        if builder.web_build_is_stale(web_dir, index):
            print("[INFO] Reused Web assets were stale; falling back to local build.")
            shutil.rmtree(web_dir / "dist", ignore_errors=True)
            return False
        print(f"[INFO] Reused Web assets from: {seed_index.parent.parent.parent}")
        return True
    except OSError as error:
        print(f"[INFO] Could not reuse Web assets ({error}); falling back to local build.")
        shutil.rmtree(web_dir / "dist", ignore_errors=True)
        return False


def refresh_web_assets(root: Path, force: bool = False) -> bool:
    module = load_web_builder(root)
    if module is None:
        print("[ERROR] Cannot load Web asset builder.", file=sys.stderr)
        return False
    if not force and seed_web_assets(root, module):
        return True
    try:
        _, pnpm = module.ensure_node_and_pnpm()
        module.build_web(root / "web", pnpm, force=force)
    except (OSError, subprocess.SubprocessError, SystemExit):
        return False
    return True


def sanitize_identity(name: str, identity: str) -> str:
    """Build the shared filesystem-safe `<name>_<commit12>` runtime identity."""
    return re.sub(r"[^A-Za-z0-9_.]", "_", f"{name}_{identity[:12]}")


def safe_identity(root: Path) -> str:
    return sanitize_identity(root.name, current_commit(root) or root.name)


def runtime_root(root: Path) -> Path:
    legacy = root / ".acecode" / "dev-run"
    return legacy if legacy.is_dir() else root / ".acecode" / "dev_run"


def worktree_runtime_dir(root: Path) -> Path:
    base = runtime_root(root)
    legacy_id = re.sub(r"[^A-Za-z0-9_.-]", "-", f"{root.name}-{(current_commit(root) or root.name)[:12]}")
    legacy = base / legacy_id
    return legacy if legacy.is_dir() else base / safe_identity(root)


def worktree_prefix(root: Path) -> str:
    """Prefix every runtime directory owned by this worktree shares."""
    return re.sub(r"[^A-Za-z0-9_.]", "_", root.name) + "_"


def selected_web_runtime_dir(root: Path, extra: list[str]) -> Path:
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--run-dir", type=Path)
    args, _ = parser.parse_known_args(extra)
    if args.run_dir is None:
        return worktree_runtime_dir(root)
    return args.run_dir.resolve() if args.run_dir.is_absolute() else (root / args.run_dir).resolve()


def selected_web_port(extra: list[str]) -> int | None:
    """Return the daemon port explicitly requested on the command line, if any."""
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--port", type=int)
    args, _ = parser.parse_known_args(extra)
    return args.port


def derived_port(identity: str) -> int:
    """Derive a stable port from the runtime identity.

    Uses zlib.crc32 because the built-in hash() is salted per process and would
    hand out a different port on every run.
    """
    digest = zlib.crc32(identity.encode("utf-8"))
    return PORT_RANGE_START + digest % PORT_RANGE_COUNT


def port_is_available(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        try:
            probe.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def select_runtime_port(run_dir: Path, extra: list[str]) -> int | None:
    """Pick the daemon port: explicit takes precedence, otherwise derive and probe.

    The derivation key is the runtime directory name, so the port changes only
    when the runtime identity does - the run-dir and its port move together.
    """
    explicit = selected_web_port(extra)
    if explicit is not None:
        if not 1 <= explicit <= 65535:
            print("[ERROR] Web daemon port must be between 1 and 65535.", file=sys.stderr)
            return None
        if port_is_available(explicit):
            return explicit
        print(f"[ERROR] Requested Web daemon port is already bound: {explicit}", file=sys.stderr)
        print("[INFO] Stop the process using it or start without --port.", file=sys.stderr)
        return None
    start = derived_port(run_dir.name)
    for offset in range(PORT_RANGE_COUNT):
        candidate = PORT_RANGE_START + (start - PORT_RANGE_START + offset) % PORT_RANGE_COUNT
        if port_is_available(candidate):
            return candidate
    print(f"[ERROR] No free Web daemon port in {PORT_RANGE_START}-{PORT_RANGE_END}.", file=sys.stderr)
    return None


def pid_is_alive(pid: int) -> bool:
    """Report whether a recorded pid still maps to a live process.

    Never signals the process: POSIX uses signal 0 as a probe and Windows opens
    it with query-only access.
    """
    if pid <= 0:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError as error:
            # Permission failures do not prove that the process is dead.
            return error.errno != errno.ESRCH
        except OverflowError:
            return True
    return windows_pid_is_alive(pid)


def windows_pid_is_alive(pid: int) -> bool:
    import ctypes
    from ctypes import wintypes

    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    if not 0 < pid <= 0xFFFFFFFF:
        return True
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        # ERROR_INVALID_PARAMETER identifies a nonexistent PID. Access denied
        # and other probe failures must preserve the possibly live runtime.
        return ctypes.get_last_error() != 87
    try:
        code = ctypes.c_ulong()
        if kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return code.value == STILL_ACTIVE
        return True
    finally:
        kernel32.CloseHandle(handle)


def runtime_pid(run_dir: Path) -> int | None:
    try:
        pid = int((run_dir / "daemon.pid").read_text(encoding="utf-8").strip())
        return pid if pid > 0 else None
    except (OSError, ValueError):
        return None


def web_launcher_options(extra: list[str]) -> tuple[bool, bool, list[str]]:
    """Return embedded mode, explicit native-build approval, and remaining args."""
    embedded = False
    build_daemon = False
    remaining: list[str] = []
    for argument in extra:
        if argument == "--embedded":
            embedded = True
        elif argument == "--build-daemon":
            build_daemon = True
        else:
            remaining.append(argument)
    return embedded, build_daemon, remaining


def vite_options(extra: list[str]) -> list[str]:
    """Strip daemon-only runtime options before forwarding options to Vite."""
    result: list[str] = []
    skip_next = False
    for argument in extra:
        if skip_next:
            skip_next = False
            continue
        if argument.startswith("--run-dir=") or argument.startswith("--port="):
            continue
        if argument in ("--run-dir", "--port"):
            skip_next = True
            continue
        result.append(argument)
    return result


def web_runtime_is_available(root: Path, candidate: BuildCandidate | None, extra: list[str]) -> bool:
    run_dir = selected_web_runtime_dir(root, extra)
    explicit = any(arg == "--run-dir" or arg.startswith("--run-dir=") for arg in extra)
    if not explicit:
        # A new commit changes the default run-dir name but an older worker
        # can still hold this build's executable open. Inspect only this
        # worktree's own launcher directories, without stopping any process.
        prefixes = (worktree_prefix(root), re.sub(r"[^A-Za-z0-9_.-]", "-", root.name) + "-")
        bases = (run_dir.parent, root / ".acecode" / "dev_run", root / ".acecode" / "dev-run")
        for base in dict.fromkeys(bases):
            if not base.is_dir():
                continue
            previous = next((path for path in sorted(base.iterdir())
                             if path.name.startswith(prefixes) and (path / "daemon.pid").exists()), None)
            if previous is not None:
                run_dir = previous
                break
    if not (run_dir / "daemon.pid").exists():
        return True
    print(f"[ERROR] Existing Web daemon runtime must be stopped before rebuilding: {run_dir}", file=sys.stderr)
    if candidate is None:
        print("[ERROR] No verified executable is available to inspect that runtime; check it manually.", file=sys.stderr)
        return False
    command = [str(candidate.executable), "daemon", "status", f"--run-dir={run_dir}"]
    try:
        result = subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=10, check=False)
        if result.returncode == 0:
            command = [command[0], "daemon", "stop", command[3]]
            print("[INFO] Stop this verified daemon, then rerun the launcher:", file=sys.stderr)
        else:
            print("[ERROR] Runtime identity is unverified; inspect it before stopping or removing anything:", file=sys.stderr)
    except (OSError, subprocess.TimeoutExpired):
        print("[ERROR] Runtime identity check failed; inspect it manually:", file=sys.stderr)
    formatted = "& " + " ".join("'" + part.replace("'", "''") + "'" for part in command) if os.name == "nt" else shlex.join(command)
    print(("PowerShell: " if os.name == "nt" else "") + formatted, file=sys.stderr)
    return False


def launch_surface(root: Path, target: str, candidate: BuildCandidate, dry_run: bool, extra: list[str]) -> int:
    if target == "web":
        run_dir = selected_web_runtime_dir(root, extra)
        command = [sys.executable, str(root / "scripts" / "dev_web.py"), "--build-dir", str(candidate.executable.parent), "--run-dir", str(run_dir), *extra]
        print(f"[INFO] Launching Web with build: {candidate.build_dir}")
        print(f"[INFO] Isolated runtime directory: {run_dir}")
    elif target == "desktop":
        command = [sys.executable, str(root / "scripts" / "dev_desktop.py"), "--build-dir", str(candidate.executable), "--no-build", *extra]
        print(f"[INFO] Launching Desktop with build: {candidate.build_dir}")
    else:
        command = tui_command(root, candidate.executable, extra)
        if command is None:
            print(f"[ERROR] Cannot open a new terminal automatically. Run: {candidate.executable}", file=sys.stderr)
            return 1
        print(f"[INFO] Launching TUI in a new terminal with build: {candidate.build_dir}")
    if dry_run:
        print("[INFO] Dry run: " + " ".join(f'"{part}"' if " " in part else part for part in command))
        return 0
    try:
        if target == "tui" and os.name == "nt":
            subprocess.Popen(command, cwd=root, creationflags=subprocess.CREATE_NEW_CONSOLE)
            return 0
        if target == "tui" and sys.platform != "darwin":
            subprocess.Popen(command, cwd=root, start_new_session=True)
            return 0
        # Surface scripts and osascript finish after creating the UI; waiting
        # propagates launcher/argument failures without waiting for the GUI.
        return subprocess.run(command, cwd=root, check=False).returncode
    except OSError as error:
        print(f"[ERROR] Could not start {target}: {error}", file=sys.stderr)
        return 1


def tui_command(root: Path, executable: Path, extra: list[str] | None = None) -> list[str] | None:
    arguments = [str(executable), *(extra or [])]
    if os.name == "nt":
        return arguments
    if sys.platform == "darwin":
        shell_command = f"cd {shlex.quote(str(root))} && exec {shlex.join(arguments)}"
        script = 'tell application "Terminal"\nactivate\ndo script ' + json.dumps(shell_command, ensure_ascii=False) + "\nend tell"
        return ["osascript", "-e", script]
    for terminal in ("x-terminal-emulator", "gnome-terminal", "konsole", "xterm"):
        path = shutil.which(terminal)
        if path:
            if terminal == "gnome-terminal":
                return [path, "--", *arguments]
            return [path, "-e", *arguments]
    return None


def daemon_port(runtime_dir: Path) -> int | None:
    try:
        port = int((runtime_dir / "daemon.port").read_text(encoding="utf-8").strip())
        return port if 1 <= port <= 65535 else None
    except (OSError, ValueError):
        return None


def daemon_worker_log_path(runtime_dir: Path) -> Path:
    return runtime_dir / WORKER_LOG_NAME


def worker_failure_detail(runtime_dir: Path, reason: str) -> str:
    worker = daemon_worker_log_path(runtime_dir)
    startup = runtime_dir / DAEMON_STARTUP_LOG_NAME
    if not worker.exists() and (runtime_dir / "daemon-worker.log").exists():
        worker = runtime_dir / "daemon-worker.log"
    if not startup.exists() and (runtime_dir / "daemon-startup.log").exists():
        startup = runtime_dir / "daemon-startup.log"
    return f"{reason}; see {worker} and {startup}"


def open_worker_log(runtime_dir: Path, command: list[str]):
    """Open the append-mode worker log, writing a header for this spawn."""
    log_path = daemon_worker_log_path(runtime_dir)
    runtime_dir.mkdir(parents=True, exist_ok=True)
    previous = log_path.with_name(f"{WORKER_LOG_NAME}.1")
    try:
        if log_path.is_file() and log_path.stat().st_size > WORKER_LOG_MAX_BYTES:
            previous.unlink(missing_ok=True)
            log_path.replace(previous)
    except OSError:
        pass  # Rotation is best-effort; never block startup on it.
    handle = log_path.open("ab")
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    # The header carries no token; only paths and the requested port.
    handle.write(f"\n===== {stamp} spawn: {' '.join(command)} =====\n".encode("utf-8", "replace"))
    handle.flush()
    return handle


def spawn_daemon_worker(root: Path, runtime_dir: Path, command: list[str]):
    """Start the daemon worker as the only place this script launches it."""
    log = open_worker_log(runtime_dir, command)
    options = {"cwd": root, "stdout": log, "stderr": subprocess.STDOUT}
    if os.name == "nt":
        options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    try:
        worker = subprocess.Popen(command, **options)
    except OSError as error:
        log.close()
        print(f"[ERROR] Could not start Web daemon: {error}", file=sys.stderr)
        return None
    log.close()
    return worker


def daemon_is_healthy(root: Path, candidate: BuildCandidate, run_dir: Path,
                      timeout_seconds: float = 0, worker=None) -> bool:
    del root, candidate
    deadline = time.monotonic() + timeout_seconds
    while True:
        # A worker that already exited will never answer /api/health; failing
        # here keeps a crash visible in seconds instead of after the timeout.
        if worker is not None and worker.poll() is not None:
            return False
        try:
            pid = int((run_dir / "daemon.pid").read_text(encoding="utf-8").strip())
            port = daemon_port(run_dir)
            token = daemon_token(run_dir)
            if pid > 0 and port is not None and token:
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/api/health",
                    headers={"X-ACECode-Token": token},
                )
                with urllib.request.urlopen(request, timeout=1) as response:
                    payload = json.loads(response.read().decode("utf-8"))
                # A stale runtime can point at another daemon already using the
                # same port. Match the daemon identity from the health payload,
                # not merely TCP reachability.
                if int(payload.get("pid", -1)) == pid and int(payload.get("port", -1)) == port:
                    return True
        except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError,
                urllib.error.URLError):
            pass
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.1)


def start_quick_web_daemon(root: Path, candidate: BuildCandidate, run_dir: Path, port: int) -> int | None:
    """Start the worker directly; the detached daemon wrapper rejects token.tmp runtimes."""
    command = [
        str(candidate.executable), "daemon", "--foreground",
        f"--cwd={root.as_posix()}", f"--run-dir={run_dir.as_posix()}", f"--port={port}",
    ]
    worker = spawn_daemon_worker(root, run_dir, command)
    if worker is None:
        return None
    if not daemon_is_healthy(root, candidate, run_dir, timeout_seconds=15, worker=worker):
        exit_code = worker.poll()
        reason = f"worker exited with code {exit_code}" if exit_code is not None else "health check timed out"
        print(f"[ERROR] {worker_failure_detail(run_dir, reason)}", file=sys.stderr)
        return None
    return daemon_port(run_dir)


def stop_stale_runtime(root: Path, candidate: BuildCandidate | None, run_dir: Path) -> bool:
    """Clear runtime files for a pid already confirmed dead. Never signals it."""
    if candidate is None:
        print("[ERROR] No verified executable is available to clean that runtime; check it manually.", file=sys.stderr)
        return False
    command = [str(candidate.executable), "daemon", "stop", f"--run-dir={run_dir.as_posix()}"]
    try:
        result = subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=15, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"[ERROR] Could not clean the stale runtime: {error}", file=sys.stderr)
        return False
    if result.returncode != 0:
        detail = result.stdout.strip() or result.stderr.strip() or f"exit code {result.returncode}"
        print(f"[ERROR] Stale runtime cleanup failed: {detail}", file=sys.stderr)
        return False
    return True


def reconcile_stale_runtime(root: Path, candidate: BuildCandidate | None, run_dir: Path) -> bool:
    """Self-heal a rejected runtime only when its recorded pid is provably dead."""
    if not (run_dir / "daemon.pid").exists():
        return True  # Nothing ever claimed this directory; start fresh.
    pid = runtime_pid(run_dir)
    if pid is None:
        print(f"[ERROR] Web daemon runtime has an unreadable pid file; inspect it before retrying: {run_dir}", file=sys.stderr)
        return False
    if pid_is_alive(pid):
        print(f"[ERROR] Existing Web daemon runtime is unhealthy; stop it manually before retrying: {run_dir}", file=sys.stderr)
        print(f"[INFO] Its recorded pid {pid} is still alive, so nothing was stopped or removed.", file=sys.stderr)
        return False
    print(f"[INFO] Recovering stale runtime whose pid {pid} is dead: {run_dir}")
    return stop_stale_runtime(root, candidate, run_dir)


def daemon_token(runtime_dir: Path) -> str | None:
    for name in ("token", "token.tmp"):
        try:
            token = (runtime_dir / name).read_text(encoding="utf-8").strip()
            if token:
                return token
        except OSError:
            pass
    return None


def launch_vite(root: Path, daemon_port_number: int, runtime_dir: Path, extra: list[str]) -> int:
    token = daemon_token(runtime_dir)
    if token is None:
        print(f"[ERROR] Web daemon did not provide an authentication token: {runtime_dir}", file=sys.stderr)
        return 1
    builder = load_web_builder(root)
    if builder is None:
        print("[ERROR] Cannot load Web development server helper.", file=sys.stderr)
        return 1
    try:
        _, pnpm = builder.ensure_node_and_pnpm()
    except (OSError, subprocess.SubprocessError, SystemExit):
        return 1
    web_dir = root / "web"
    if not (web_dir / "node_modules").is_dir():
        print("[INFO] Installing Web dependencies...")
        if subprocess.run([pnpm, "install"], cwd=web_dir, check=False).returncode != 0:
            return 1
    environment = os.environ.copy()
    environment["ACECODE_DAEMON_PORT"] = str(daemon_port_number)
    environment["ACECODE_DAEMON_TOKEN"] = token
    print(f"[INFO] Starting Vite with daemon proxy: http://127.0.0.1:{daemon_port_number}")
    return subprocess.run([pnpm, "dev", *vite_options(extra)], cwd=web_dir, env=environment, check=False).returncode


def prune_candidates(root: Path) -> list[Path]:
    """Runtime directories worth inspecting: the worktree root plus the legacy tree."""
    candidates: list[Path] = []
    for name in ("dev_run", "dev-run"):
        base = root / ".acecode" / name
        if base.is_dir() and base.resolve().is_relative_to(root.resolve()):
            candidates.extend(sorted(path for path in base.iterdir()))
    legacy = root / ".acecode-dev-run"
    if legacy.is_dir() and legacy.resolve().is_relative_to(root.resolve()):
        candidates.append(legacy)
    return candidates


def classify_runtime_directory(directory: Path) -> tuple[bool, str]:
    """Return whether the directory can be deleted and the reason why (not)."""
    if not directory.is_dir():
        return False, "not a directory"
    if directory.is_symlink() or getattr(directory, "is_junction", lambda: False)():
        return False, "linked runtime directory"
    if directory.resolve().parent != directory.parent.resolve():
        return False, "runtime path escapes its parent"
    pid = runtime_pid(directory)
    if pid is None:
        return False, "no readable daemon.pid"
    if pid_is_alive(pid):
        return False, f"pid {pid} is alive"
    return True, str(pid)


def prune_runtime_directories(directories: Iterable[Path]) -> tuple[list[tuple[Path, str]], list[tuple[Path, str]]]:
    """Delete only directories whose recorded pid is confirmed dead."""
    pruned: list[tuple[Path, str]] = []
    skipped: list[tuple[Path, str]] = []
    for directory in directories:
        deletable, detail = classify_runtime_directory(directory)
        if not deletable:
            skipped.append((directory, detail))
            continue
        try:
            shutil.rmtree(directory)
        except OSError as error:
            skipped.append((directory, f"remove failed: {error}"))
            continue
        pruned.append((directory, detail))
    return pruned, skipped


def prune_runtime_tree(root: Path, dry_run: bool = False) -> int:
    candidates = prune_candidates(root)
    if dry_run:
        verdicts = [classify_runtime_directory(path) for path in candidates]
        count = sum(1 for deletable, _ in verdicts if deletable)
        print(f"[INFO] Dry run: pruning would remove {count} of {len(verdicts)} inspected path(s).")
        return 0
    pruned, skipped = prune_runtime_directories(candidates)
    print("[INFO] Pruned:")
    for directory, pid in pruned or []:
        print(f"  {directory} (dead pid={pid})")
    if not pruned:
        print("  (none)")
    print("[INFO] Skipped:")
    for directory, reason in skipped or []:
        print(f"  {directory} ({reason})")
    if not skipped:
        print("  (none)")
    print(f"[INFO] Total: pruned {len(pruned)}, skipped {len(skipped)}")
    return 0


def prune_worktree_runtime_dirs(root: Path, current: Path) -> int:
    """Drop this worktree's own runtime directories left behind by dead pids."""
    base = runtime_root(root)
    if not base.is_dir() or not base.resolve().is_relative_to(root.resolve()):
        return 0
    prefix = (worktree_prefix(root), re.sub(r"[^A-Za-z0-9_.-]", "-", root.name) + "-")
    stale = [
        previous for previous in sorted(base.iterdir())
        if previous.is_dir() and previous.name != current.name and previous.name.startswith(prefix)
    ]
    pruned, _ = prune_runtime_directories(stale)
    for directory, pid in pruned:
        print(f"[INFO] Removed stale runtime for dead pid {pid}: {directory}")
    return len(pruned)


def launch_quick_web(root: Path, candidate: BuildCandidate, extra: list[str]) -> int:
    run_dir = selected_web_runtime_dir(root, extra)
    if daemon_is_healthy(root, candidate, run_dir):
        port = daemon_port(run_dir)
        print(f"[INFO] Reusing Web daemon: http://127.0.0.1:{port}")
        return launch_vite(root, port, run_dir, extra)
    if not reconcile_stale_runtime(root, candidate, run_dir):
        return 1
    prune_worktree_runtime_dirs(root, run_dir)
    port = select_runtime_port(run_dir, extra)
    if port is None:
        return 1
    started = start_quick_web_daemon(root, candidate, run_dir, port)
    if started is None:
        print("[ERROR] Web daemon could not be started.", file=sys.stderr)
        return 1
    print(f"[INFO] Started Web daemon: http://127.0.0.1:{started}")
    return launch_vite(root, started, run_dir, extra)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Start an ACECode development environment", allow_abbrev=False)
    parser.add_argument("target", nargs="?", choices=TARGETS, help="development surface to start, or prune to reclaim stale runtimes")
    parser.add_argument("--build-dir", type=Path, help="build directory to validate and use")
    parser.add_argument("--yes", action="store_true", help="confirm a required CMake build")
    parser.add_argument("--dry-run", action="store_true", help="print the selected command without starting it")
    args, extra = parser.parse_known_args()
    args.extra = extra
    return args


def choose_target(target: str | None) -> str | None:
    if target:
        return target
    if sys.stdin is None or not sys.stdin.isatty():
        print("[ERROR] Specify one target: web, desktop, or tui.", file=sys.stderr)
        return None
    selected = input("Choose development target [web/desktop/tui]: ").strip().lower()
    if selected not in TARGETS:
        print("[ERROR] Choose web, desktop, or tui.", file=sys.stderr)
        return None
    return selected


def main() -> int:
    args = parse_args()
    target = choose_target(args.target)
    if not target:
        return 2
    root = project_root()
    if target == PRUNE_TARGET:
        return prune_runtime_tree(root, dry_run=args.dry_run)
    embedded, build_daemon, extra = web_launcher_options(args.extra) if target == "web" else (False, False, args.extra)
    if target == "web" and not embedded:
        if "--yes" in extra:
            print("[ERROR] --yes is only available to the embedded validation workflow; use --build-daemon for quick mode.", file=sys.stderr)
            return 2
        candidate = find_compatible_build(root, target, args.build_dir)
        if args.build_dir and candidate is None:
            print(f"[ERROR] --build-dir does not contain a compatible configured {target} build: {(root / args.build_dir).resolve()}", file=sys.stderr)
            return 1
        if candidate is None:
            preset = default_preset(target)
            if preset is None:
                print("[ERROR] No supported CMake preset for this platform and architecture.", file=sys.stderr)
                return 1
            if not build_daemon and (sys.stdin is None or not sys.stdin.isatty()):
                print("[ERROR] No compatible daemon executable was found. Re-run with --build-daemon to authorize its native build.", file=sys.stderr)
                return 1
            if not ask_to_build(preset, target, build_daemon):
                return 1
            if args.dry_run:
                print(f"[INFO] Dry run: cmake --preset {preset}")
                return 0
            if not ensure_windows_environment(root, None):
                return 1
            sccache = find_sccache()
            built = configure_and_build(root, preset, target, sccache)
            if not built and sccache:
                print("[INFO] sccache configuration failed; retrying normal compilation.")
                built = configure_and_build(root, preset, target, None)
            if not built:
                return 1
            candidate = find_compatible_build(root, target, built)
            if candidate is None:
                print("[ERROR] Build completed but did not produce a compatible executable.", file=sys.stderr)
                return 1
        if args.dry_run:
            print(f"[INFO] Dry run: start Vite with {candidate.executable}")
            return 0
        return launch_quick_web(root, candidate, extra)
    if target == "desktop" and "--list" in args.extra:
        command = [sys.executable, str(root / "scripts/dev_desktop.py"), *args.extra]
        if args.build_dir:
            command.extend(("--build-dir", str(args.build_dir)))
        if args.dry_run:
            print("[INFO] Dry run: " + subprocess.list2cmdline(command))
            return 0
        return subprocess.run(command, cwd=root, check=False).returncode
    sccache = find_sccache()
    if sccache:
        print(f"[INFO] Using sccache: {sccache}")
    else:
        print(f"[INFO] sccache not found. {sccache_install_hint()}")
    candidate = find_compatible_build(root, target, args.build_dir)
    if args.build_dir and candidate is None:
        print(f"[ERROR] --build-dir does not contain a compatible configured {target} build: {(root / args.build_dir).resolve()}", file=sys.stderr)
        return 1
    if not args.dry_run and target == "web" and not web_runtime_is_available(root, candidate, extra):
        return 1
    if not args.dry_run and not ensure_windows_environment(root, candidate):
        return 1
    sccache_disabled = candidate is not None and sccache_disabled_for_build(candidate.build_dir, sccache)
    if sccache_disabled:
        print("[INFO] sccache is disabled for this build after a prior compiler failure.")
        sccache = None
    if candidate is None:
        preset = default_preset(target)
        if preset is None:
            print("[ERROR] No supported CMake preset for this platform and architecture.", file=sys.stderr)
            return 1
        if not ask_to_build(preset, target, args.yes or (target == "web" and embedded and os.name == "nt")):
            return 1
        if args.dry_run:
            print(f"[INFO] Dry run: cmake --preset {preset}")
            return 0
        built = configure_and_build(root, preset, target, sccache)
        if not built:
            if sccache:
                print("[INFO] sccache configuration failed; retrying normal compilation.")
                built = configure_and_build(root, preset, target, None)
            if not built:
                return 1
        candidate = find_compatible_build(root, target, built)
        if candidate is None:
            print("[ERROR] Build completed but did not produce a compatible executable.", file=sys.stderr)
            return 1
    if not args.dry_run and not sccache_disabled and cache_state_changed(candidate.build_dir, sccache):
        preset = default_preset(target)
        if preset is None:
            print("[ERROR] Cannot reconfigure the build for this platform.", file=sys.stderr)
            return 1
        if not configure_build(root, preset, candidate.build_dir, sccache):
            if sccache:
                print("[INFO] sccache reconfiguration failed; retrying without it.")
                sccache = None
                if not configure_build(root, preset, candidate.build_dir, None):
                    return 1
            else:
                return 1
    if not args.dry_run and target == "web":
        if not refresh_web_assets(root):
            print("[ERROR] Web asset refresh failed; embedded development environment was not started.", file=sys.stderr)
            return 1
    if not args.dry_run and not build_target(root, candidate.build_dir, target, sccache, candidate.configuration):
        if sccache:
            preset = default_preset(target)
            print("[INFO] sccache build failed; retrying this build without sccache.")
            if preset and configure_build(root, preset, candidate.build_dir, None) and build_target(root, candidate.build_dir, target, None, candidate.configuration):
                write_sccache_marker(candidate.build_dir, find_sccache(), enabled=False)
                sccache = None
            else:
                print("[ERROR] Incremental build failed; development environment was not started.", file=sys.stderr)
                return 1
        else:
            print("[ERROR] Incremental build failed; development environment was not started.", file=sys.stderr)
            return 1
    if target == "desktop" and not args.dry_run:
        refreshed = refresh_web_assets(root, force=True) if "--rebuild" in args.extra else refresh_web_assets(root)
        if not refreshed:
            print("[ERROR] Web asset refresh failed; development environment was not started.", file=sys.stderr)
            return 1
    return launch_surface(root, target, candidate, args.dry_run, extra)


if __name__ == "__main__":
    raise SystemExit(main())
