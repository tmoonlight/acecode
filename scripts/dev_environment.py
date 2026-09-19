#!/usr/bin/env python3
"""Launch ACECode Web, Desktop, or TUI development environments safely."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from dev_build_artifacts import find_named_artifacts

TARGETS = ("web", "desktop", "tui")
CACHE_MARKER = ".acecode-sccache.json"


@dataclass(frozen=True)
class BuildCandidate:
    build_dir: Path
    source_dir: Path
    executable: Path


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def native_executable_name(name: str) -> str:
    return f"{name}.exe" if os.name == "nt" else name


def executable_for(build_dir: Path, target: str) -> Path | None:
    name = native_executable_name("acecode-desktop" if target == "desktop" else "acecode")
    matches = find_named_artifacts(build_dir, [name], "ACECode.app" if target == "desktop" else None)
    return matches[0] if matches else None


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
    expected_system = "windows" if system == "windows" else "osx" if system == "darwin" else "linux"
    machine = platform.machine().lower()
    expected_arch = "arm64" if machine in {"arm64", "aarch64"} else "x64" if machine in {"amd64", "x86_64"} else None
    triplet_lower = triplet.lower()
    return expected_system in triplet_lower and (expected_arch is None or expected_arch in triplet_lower)


def find_compatible_build(root: Path, target: str, explicit: Path | None = None) -> BuildCandidate | None:
    root = root.resolve()
    directories = [explicit.resolve()] if explicit else sorted(
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
        return BuildCandidate(build_dir.resolve(), source_dir, executable)
    return None


def default_preset(target: str) -> str | None:
    system = platform.system().lower()
    machine = platform.machine().lower()
    arch = "arm64" if machine in {"arm64", "aarch64"} else "x64" if machine in {"amd64", "x86_64"} else None
    if not arch:
        return None
    prefix = {"windows": "windows", "darwin": "macos", "linux": "linux"}.get(system)
    if not prefix:
        return None
    suffix = "-desktop-release" if target == "desktop" else "-release"
    return f"{prefix}-{arch}{suffix}"


def ask_to_build(preset: str, target: str, assume_yes: bool) -> bool:
    binary_dir = f"build/{preset}"
    executable_target = "acecode-desktop" if target == "desktop" else "acecode"
    print("[INFO] No compatible build was found in this repository's registered worktrees.")
    print(f"[INFO] Proposed configure preset: {preset} (BUILD_TESTING=OFF)")
    print(f"[INFO] Proposed build: cmake --build {binary_dir} --target {executable_target}")
    if assume_yes:
        return True
    if not sys.stdin.isatty():
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
            "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
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


def build_target(root: Path, build_dir: Path, target: str, sccache: Path | None = None) -> bool:
    executable_target = "acecode-desktop" if target == "desktop" else "acecode"
    before = sccache_stats(sccache) if sccache else None
    result = subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", executable_target],
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


def refresh_web_assets(root: Path) -> bool:
    module = load_web_builder(root)
    if module is None:
        print("[ERROR] Cannot load Web asset builder.", file=sys.stderr)
        return False
    if seed_web_assets(root, module):
        return True
    try:
        _, pnpm = module.ensure_node_and_pnpm()
        module.build_web(root / "web", pnpm)
    except (OSError, subprocess.SubprocessError, SystemExit):
        return False
    return True


def worktree_runtime_dir(root: Path) -> Path:
    identity = current_commit(root) or root.name
    safe = re.sub(r"[^A-Za-z0-9_.-]", "-", f"{root.name}-{identity[:12]}")
    return root / ".acecode" / "dev-run" / safe


def launch_surface(root: Path, target: str, candidate: BuildCandidate, dry_run: bool, extra: list[str]) -> int:
    if target == "web":
        run_dir = worktree_runtime_dir(root)
        command = [sys.executable, str(root / "scripts" / "dev_web.py"), "--build-dir", str(candidate.build_dir), "--run-dir", str(run_dir), *extra]
        print(f"[INFO] Launching Web with build: {candidate.build_dir}")
        print(f"[INFO] Isolated runtime directory: {run_dir}")
    elif target == "desktop":
        command = [sys.executable, str(root / "scripts" / "dev_desktop.py"), "--build-dir", str(candidate.build_dir), "--no-build", *extra]
        print(f"[INFO] Launching Desktop with build: {candidate.build_dir}")
    else:
        command = tui_command(root, candidate.executable)
        if command is None:
            print(f"[ERROR] Cannot open a new terminal automatically. Run: {candidate.executable}", file=sys.stderr)
            return 1
        print(f"[INFO] Launching TUI in a new terminal with build: {candidate.build_dir}")
    if dry_run:
        print("[INFO] Dry run: " + " ".join(f'"{part}"' if " " in part else part for part in command))
        return 0
    return subprocess.Popen(command, cwd=root).wait() if target == "web" else (subprocess.Popen(command, cwd=root) and 0)


def tui_command(root: Path, executable: Path) -> list[str] | None:
    if os.name == "nt":
        return ["cmd.exe", "/d", "/c", "start", "ACECode TUI", "/d", str(root), str(executable)]
    if sys.platform == "darwin":
        return ["open", "-a", "Terminal", str(executable)]
    for terminal in ("x-terminal-emulator", "gnome-terminal", "konsole", "xterm"):
        path = shutil.which(terminal)
        if path:
            if terminal == "gnome-terminal":
                return [path, "--", str(executable)]
            if terminal == "konsole":
                return [path, "-e", str(executable)]
            return [path, "-e", str(executable)]
    return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Start an ACECode development environment")
    parser.add_argument("target", nargs="?", choices=TARGETS, help="development surface to start")
    parser.add_argument("--build-dir", type=Path, help="build directory to validate and use")
    parser.add_argument("--yes", action="store_true", help="confirm a required CMake build")
    parser.add_argument("--dry-run", action="store_true", help="print the selected command without starting it")
    args, extra = parser.parse_known_args()
    args.extra = extra
    return args


def choose_target(target: str | None) -> str | None:
    if target:
        return target
    if not sys.stdin.isatty():
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
    sccache = find_sccache()
    if sccache:
        print(f"[INFO] Using sccache: {sccache}")
    else:
        print(f"[INFO] sccache not found. {sccache_install_hint()}")
    candidate = find_compatible_build(root, target, args.build_dir)
    sccache_disabled = candidate is not None and sccache_disabled_for_build(candidate.build_dir, sccache)
    if sccache_disabled:
        print("[INFO] sccache is disabled for this build after a prior compiler failure.")
        sccache = None
    if candidate is None:
        preset = default_preset(target)
        if preset is None:
            print("[ERROR] No supported CMake preset for this platform and architecture.", file=sys.stderr)
            return 1
        if not ask_to_build(preset, target, args.yes):
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
    if not args.dry_run and not build_target(root, candidate.build_dir, target, sccache):
        if sccache:
            preset = default_preset(target)
            print("[INFO] sccache build failed; retrying this build without sccache.")
            if preset and configure_build(root, preset, candidate.build_dir, None) and build_target(root, candidate.build_dir, target, None):
                write_sccache_marker(candidate.build_dir, find_sccache(), enabled=False)
                sccache = None
            else:
                print("[ERROR] Incremental build failed; development environment was not started.", file=sys.stderr)
                return 1
        else:
            print("[ERROR] Incremental build failed; development environment was not started.", file=sys.stderr)
            return 1
    if target in {"web", "desktop"} and not args.dry_run and not refresh_web_assets(root):
        print("[ERROR] Web asset refresh failed; development environment was not started.", file=sys.stderr)
        return 1
    return launch_surface(root, target, candidate, args.dry_run, args.extra)


if __name__ == "__main__":
    raise SystemExit(main())
