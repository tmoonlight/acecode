#!/usr/bin/env python3
"""Build, stage, and locally verify ACECode packages with zero release side effects.

One command reproduces the CI package layout (binaries + share/acecode resources),
checks bundled resources, and smoke-runs the staged TUI / desktop binaries.
Performs no git, publishing, signing, or network operations of its own, and
writes only inside the repo build/staging directories and the system temp dir.

Exit codes: 0 every check passed; 1 at least one check failed or could not run;
2 command-line usage error.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MODELS_DEV_FILES = ("api.json", "MANIFEST.json", "LICENSE")
README_FILES = ("README.md", "README_CN.md")
BUILD_CONFIGS = ("MinSizeRel", "Release", "RelWithDebInfo", "Debug")
DEFAULT_LAUNCH_TIMEOUT = 10
STAGING_DIRNAME = "verify-package-staging"


class Report:
    def __init__(self) -> None:
        self.items: list[tuple[str, str, str]] = []

    def add(self, name: str, status: str, detail: str = "") -> None:
        self.items.append((name, status, detail))
        mark = {"pass": "[PASS]", "fail": "[FAIL]", "skip": "[SKIP]"}[status]
        line = f"{mark} {name}"
        if detail:
            line += f": {detail}"
        print(line)

    @property
    def failed(self) -> int:
        return sum(1 for _, status, _ in self.items if status == "fail")


def find_repo_root(start: Path) -> Path:
    for candidate in [start, *start.parents]:
        if (candidate / "CMakeLists.txt").is_file() and (candidate / "assets").is_dir():
            return candidate
    raise SystemExit(
        "verify_package: unable to locate the ACECode repo root "
        "(a directory holding CMakeLists.txt and assets/) above "
        f"{start}; pass --repo explicitly."
    )


def detect_platform(explicit: str) -> str:
    if explicit != "auto":
        return explicit
    if sys.platform == "darwin":
        return "darwin"
    if sys.platform == "win32":
        return "windows"
    return "linux"


def exe_name(platform: str, kind: str) -> str:
    base = {"tui": "acecode", "desktop": "acecode-desktop", "daemon": "acecode"}[kind]
    return base + (".exe" if platform == "windows" else "")


def find_built(build_dir: Path, relative: str) -> Path | None:
    candidates = [build_dir / relative]
    candidates += [build_dir / config / relative for config in BUILD_CONFIGS]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_models_dev(report: Report, label: str, models_dir: Path, assets_dir: Path) -> None:
    if not models_dir.is_dir():
        report.add(label, "fail", f"missing directory {models_dir}")
        return
    packaged = {
        path.relative_to(models_dir).as_posix(): sha256_file(path)
        for path in sorted(models_dir.rglob("*"))
        if path.is_file()
    }
    if set(packaged) != set(MODELS_DEV_FILES):
        report.add(
            label,
            "fail",
            f"expected exactly {list(MODELS_DEV_FILES)}, found {sorted(packaged)}",
        )
        return
    mismatched = [
        name for name in MODELS_DEV_FILES
        if packaged[name] != sha256_file(assets_dir / name)
    ]
    if mismatched:
        report.add(label, "fail", f"hash mismatch vs source assets: {mismatched}")
        return
    report.add(label, "pass", f"{len(packaged)} files hash-matched")


def check_seed_bundle(report: Report, label: str, repo: Path, packaged: Path) -> None:
    script = repo / "scripts" / "verify_seed_bundle.py"
    if not script.is_file():
        report.add(label, "fail", f"missing {script}")
        return
    result = subprocess.run(
        [sys.executable, str(script), "--source", str(repo / "assets" / "seed"),
         "--packaged", str(packaged)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().splitlines()
        report.add(label, "fail", detail[-1] if detail else f"exit {result.returncode}")
        return
    report.add(label, "pass")


def run_tool(report: Report, name: str, command: list[str], **kwargs) -> bool:
    try:
        result = subprocess.run(command, **kwargs)
    except FileNotFoundError as error:
        report.add(name, "fail", str(error))
        return False
    if result.returncode != 0:
        output = ""
        if isinstance(result, subprocess.CompletedProcess):
            text = (result.stderr or result.stdout or "")
            if isinstance(text, bytes):
                text = text.decode(errors="replace")
            output = text.strip().splitlines()[-1] if text.strip() else ""
        report.add(name, "fail", f"exit {result.returncode}" + (f": {output}" if output else ""))
        return False
    report.add(name, "pass")
    return True


def preflight(report: Report, repo: Path, cmake: str | None, skip_build: bool,
              build_dir: Path) -> bool:
    ok = True
    web_dist = repo / "web" / "dist" / "index.html"
    if web_dist.is_file():
        report.add("preflight web/dist", "pass")
    else:
        report.add(
            "preflight web/dist", "fail",
            "web/dist/index.html is missing; CMake would embed a placeholder page. "
            "Run: cd web && pnpm install --frozen-lockfile && pnpm build",
        )
        ok = False
    if cmake is None:
        report.add("preflight cmake", "fail", "cmake not found on PATH")
        ok = False
    else:
        report.add("preflight cmake", "pass")
    if skip_build and not (build_dir / "CMakeCache.txt").is_file():
        report.add(
            "preflight build dir", "fail",
            f"{build_dir} is not configured; run without --skip-build",
        )
        ok = False
    else:
        report.add("preflight build dir", "pass")
    return ok


def configure_and_build(report: Report, repo: Path, build_dir: Path, cmake: str,
                        targets: list[str]) -> bool:
    if not (build_dir / "CMakeCache.txt").is_file():
        command = [cmake, "-S", str(repo), "-B", str(build_dir),
                   "-DCMAKE_BUILD_TYPE=MinSizeRel", "-DBUILD_TESTING=OFF",
                   "-DACECODE_BUILD_DESKTOP=ON"]
        if shutil.which("ninja"):
            command.insert(4, "-G")
            command.insert(5, "Ninja")
        vcpkg_root = os.environ.get("VCPKG_ROOT")
        if vcpkg_root:
            command += ["-DCMAKE_TOOLCHAIN_FILE",
                        str(Path(vcpkg_root) / "scripts" / "buildsystems" / "vcpkg.cmake")]
        if not run_tool(report, "cmake configure", command):
            return False
    else:
        report.add("cmake configure", "skip", "build dir already configured")
    for target in targets:
        if not run_tool(report, f"cmake build {target}",
                        [cmake, "--build", str(build_dir), "--config", "MinSizeRel",
                         "--target", target]):
            return False
    return True


def stage(report: Report, repo: Path, build_dir: Path, staging: Path,
          platform: str, targets: list[str], cmake: str) -> bool:
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    for name in README_FILES:
        if (repo / name).is_file():
            shutil.copy2(repo / name, staging / name)

    if "tui" in targets:
        tui_src = find_built(build_dir, exe_name(platform, "tui"))
        if tui_src is None:
            report.add("stage tui binary", "fail",
                       f"no built {exe_name(platform, 'tui')} under {build_dir}")
            return False
        shutil.copy2(tui_src, staging / exe_name(platform, "tui"))
        report.add("stage tui binary", "pass", str(tui_src))

    if "desktop" in targets:
        if platform == "darwin":
            app_src = find_built(build_dir, "ACECode.app")
            if app_src is None or not app_src.is_dir():
                report.add("stage desktop bundle", "fail",
                           f"no built ACECode.app under {build_dir}; "
                           "build the acecode-desktop target first")
                return False
            shutil.copytree(app_src, staging / "ACECode.app")
            report.add("stage desktop bundle", "pass", str(app_src))
        else:
            desktop_src = find_built(build_dir, exe_name(platform, "desktop"))
            daemon_src = find_built(build_dir, exe_name(platform, "daemon"))
            if desktop_src is None or daemon_src is None:
                report.add("stage desktop binary", "fail",
                           f"missing built {exe_name(platform, 'desktop')} or "
                           f"{exe_name(platform, 'daemon')} under {build_dir}")
                return False
            shutil.copy2(desktop_src, staging / exe_name(platform, "desktop"))
            shutil.copy2(daemon_src, staging / exe_name(platform, "daemon"))
            report.add("stage desktop binary", "pass", str(desktop_src))

    if "tui" in targets or platform != "darwin":
        for component in ("models_dev_registry", "default_seed_bundle"):
            if not run_tool(report, f"cmake install {component}",
                            [cmake, "--install", str(build_dir),
                             "--config", "MinSizeRel", "--prefix", str(staging),
                             "--component", component]):
                return False
    return True


def structural_checks(report: Report, repo: Path, staging: Path,
                      platform: str, targets: list[str]) -> None:
    if "tui" in targets or platform != "darwin":
        check_models_dev(report, "models_dev registry (staged share/)",
                         staging / "share" / "acecode" / "models_dev",
                         repo / "assets" / "models_dev")
        check_seed_bundle(report, "seed bundle (staged share/)", repo,
                          staging / "share" / "acecode" / "seed")
    if "desktop" not in targets:
        return
    if platform == "darwin":
        bundle = staging / "ACECode.app"
        contents = bundle / "Contents"
        for required in (contents / "MacOS" / "ACECode",
                         contents / "MacOS" / "acecode-daemon"):
            if required.is_file():
                report.add(f"app bundle {required.name}", "pass")
            else:
                report.add(f"app bundle {required.name}", "fail",
                           f"missing {required}")
        check_models_dev(report, "models_dev registry (app bundle)",
                         contents / "Resources" / "share" / "acecode" / "models_dev",
                         repo / "assets" / "models_dev")
        check_seed_bundle(report, "seed bundle (app bundle)", repo,
                          contents / "Resources" / "share" / "acecode" / "seed")
    else:
        daemon = staging / exe_name(platform, "daemon")
        if daemon.is_file():
            report.add("desktop daemon adjacency", "pass")
        else:
            report.add("desktop daemon adjacency", "fail",
                       f"desktop needs {daemon} beside it")


def isolated_profile_env() -> tuple[dict[str, str], Path]:
    home = Path(tempfile.mkdtemp(prefix="acecode-verify-home-"))
    env = os.environ.copy()
    env["HOME"] = str(home)
    env["USERPROFILE"] = str(home)
    if sys.platform == "win32":
        roaming = home / "AppData" / "Roaming"
        local = home / "AppData" / "Local"
        roaming.mkdir(parents=True, exist_ok=True)
        local.mkdir(parents=True, exist_ok=True)
        env["APPDATA"] = str(roaming)
        env["LOCALAPPDATA"] = str(local)
    return env, home


def probe_tui(report: Report, staged_exe: Path, platform: str) -> None:
    env, home = isolated_profile_env()
    try:
        version = subprocess.run([str(staged_exe), "--version"], capture_output=True,
                                 text=True, env=env, timeout=120)
        if version.returncode != 0 or "acecode v" not in version.stdout:
            detail = (version.stderr or version.stdout).strip().splitlines()
            report.add("tui --version", "fail",
                       detail[-1] if detail else f"exit {version.returncode}")
        else:
            report.add("tui --version", "pass", version.stdout.strip().splitlines()[0])

        registry = subprocess.run([str(staged_exe), "--validate-models-registry"],
                                  capture_output=True, text=True, env=env, timeout=300)
        output = registry.stdout + registry.stderr
        if registry.returncode != 0 or "registry OK" not in output:
            detail = output.strip().splitlines()
            report.add("tui models registry resolution", "fail",
                       detail[-1] if detail else f"exit {registry.returncode}")
        elif "models_dev" not in output.replace("\\", "/"):
            report.add("tui models registry resolution", "fail",
                       f"registry resolved away from the staged share/ tree: "
                       f"{output.strip().splitlines()[0]}")
        else:
            report.add("tui models registry resolution", "pass",
                       output.strip().splitlines()[0])
    except (subprocess.TimeoutExpired, OSError) as error:
        report.add("tui runtime probe", "fail", str(error))
    finally:
        shutil.rmtree(home, ignore_errors=True)


def existing_instance_running(platform: str) -> bool | None:
    try:
        if platform == "windows":
            result = subprocess.run(
                ["tasklist", "/FI", "IMAGENAME eq acecode-desktop.exe"],
                capture_output=True, text=True, timeout=30)
            return "acecode-desktop.exe" in (result.stdout or "")
        if platform == "darwin":
            result = subprocess.run(["pgrep", "-x", "ACECode"],
                                    capture_output=True, text=True, timeout=30)
            return result.returncode == 0
        result = subprocess.run(["pgrep", "-x", "acecode-desktop"],
                                capture_output=True, text=True, timeout=30)
        return result.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return None


def probe_desktop(report: Report, staged: Path, platform: str,
                  launch_timeout: int) -> None:
    running = existing_instance_running(platform)
    if running:
        report.add("desktop launch", "skip",
                   "an ACECode desktop instance is already running; the "
                   "single-instance guard would make this probe exit "
                   "immediately. Close ACECode and re-run to exercise startup.")
        return
    if platform == "darwin":
        command = staged / "ACECode.app" / "Contents" / "MacOS" / "ACECode"
    else:
        command = staged / exe_name(platform, "desktop")
    env, home = isolated_profile_env()
    try:
        process = subprocess.Popen([str(command)], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as error:
        report.add("desktop launch", "fail", str(error))
        shutil.rmtree(home, ignore_errors=True)
        return
    deadline = time.monotonic() + launch_timeout
    while time.monotonic() < deadline and process.poll() is None:
        time.sleep(0.25)
    if process.poll() is None:
        # Process survived the entire launch window: treat startup as
        # successful, then stop it.
        report.add("desktop launch", "pass",
                   f"alive after launch (pid {process.pid}); terminating")
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
    else:
        # Process exited at any point inside the window, including exit code 0.
        # Verdicts must wait out the window instead of passing on the first
        # poll() is None: right after spawn an immediately-exiting process
        # (e.g. an `exit 0` stub) can still report poll() is None and would be
        # wrongly marked "alive" (TOCTOU race). Waiting the full window makes
        # the immediate-exit detection deterministic.
        code = process.returncode
        if code == 0:
            report.add("desktop launch", "fail", "exited immediately with code 0")
        else:
            report.add("desktop launch", "fail",
                       f"exited with code {code} during startup")
    shutil.rmtree(home, ignore_errors=True)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build, stage, and locally verify ACECode packages "
                    "(no release side effects).")
    parser.add_argument("--target", choices=("tui", "desktop", "all"), default="all",
                        help="which artifact set to verify (default: all)")
    parser.add_argument("--skip-build", action="store_true",
                        help="reuse the existing build tree; stage and verify only")
    parser.add_argument("--platform", choices=("auto", "darwin", "windows", "linux"),
                        default="auto",
                        help="override platform detection (mainly for tests)")
    parser.add_argument("--repo", type=Path, default=None,
                        help="ACECode repo root (default: detected from this script)")
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="CMake build directory (default: <repo>/build)")
    parser.add_argument("--staging-dir", type=Path, default=None,
                        help="staging output directory "
                             "(default: <build-dir>/verify-package-staging)")
    parser.add_argument("--launch-timeout", type=int, default=DEFAULT_LAUNCH_TIMEOUT,
                        help="seconds to wait for the desktop app to stay alive "
                             "(default: 10)")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = Report()
    repo = (args.repo or find_repo_root(Path(__file__).resolve())).resolve()
    build_dir = (args.build_dir or repo / "build").resolve()
    staging = (args.staging_dir or build_dir / STAGING_DIRNAME).resolve()
    platform = detect_platform(args.platform)
    cmake = shutil.which("cmake")
    targets = ["tui", "desktop"] if args.target == "all" else [args.target]
    print(f"verify-package: repo={repo} build={build_dir} platform={platform} "
          f"target={args.target} skip-build={args.skip_build}")

    if not preflight(report, repo, cmake, args.skip_build, build_dir):
        print(f"verify-package: FAIL ({report.failed} check(s) failed)")
        return 1

    if not args.skip_build:
        assert cmake is not None
        if not configure_and_build(report, repo, build_dir, cmake, targets):
            print(f"verify-package: FAIL ({report.failed} check(s) failed)")
            return 1

    if not stage(report, repo, build_dir, staging, platform, targets, cmake or "cmake"):
        print(f"verify-package: FAIL ({report.failed} check(s) failed)")
        return 1

    structural_checks(report, repo, staging, platform, targets)

    staged_tui = staging / exe_name(platform, "tui")
    if "tui" in targets and staged_tui.is_file():
        probe_tui(report, staged_tui, platform)
    elif "tui" in targets:
        report.add("tui runtime probe", "fail", f"missing staged {staged_tui}")

    if "desktop" in targets:
        probe_desktop(report, staging, platform, args.launch_timeout)

    failed = report.failed
    if failed == 0:
        print(f"verify-package: PASS ({len(report.items)} checks) - staging at {staging}")
        return 0
    print(f"verify-package: FAIL ({failed} of {len(report.items)} checks failed)")
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
