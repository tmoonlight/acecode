#!/usr/bin/env python3
"""
ACECode Desktop 一键开发脚本

用途：只修改了 web/H5 代码时，快速启动 desktop 开发环境验证，
      无需重新编译 C++。

原理：desktop 内置 dev 模式 —— 检测到 web/dist/ 目录后，
      daemon 会从文件系统加载前端资源，pnpm build 后按 F5 即生效。

用法：
  python scripts/dev_desktop.py              # 自动构建(如需)并启动 desktop
  python scripts/dev_desktop.py --rebuild    # 强制重新构建 web 再启动
  python scripts/dev_desktop.py --no-build   # 跳过构建，直接启动
  python scripts/dev_desktop.py --build-dir <path>  # 指定 desktop 构建目录
  python scripts/dev_desktop.py --list       # 列出可用的 desktop 构建产物
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


# ─── 颜色输出（终端支持时） ───────────────────────────────────────────────

def _supports_color() -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    if sys.platform == "win32":
        return os.environ.get("WT_SESSION") is not None or os.environ.get("ANSICON") is not None
    return sys.stdout.isatty()


_COLOR = _supports_color()


def _c(text: str, code: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _COLOR else text


def info(msg: str) -> None:
    print(_c("[INFO] ", "36") + msg)


def ok(msg: str) -> None:
    print(_c("[OK]   ", "32") + msg)


def warn(msg: str) -> None:
    print(_c("[WARN] ", "33") + msg)


def error(msg: str) -> None:
    print(_c("[ERROR]", "31") + " " + msg, file=sys.stderr)


def banner(msg: str) -> None:
    width = max(len(msg) + 4, 40)
    print(_c("=" * width, "35"))
    print(_c(f"  {msg}", "35"))
    print(_c("=" * width, "35"))


# ─── 项目根目录定位 ────────────────────────────────────────────────────────

def find_project_root() -> Path:
    """从脚本位置向上查找项目根目录（包含 CMakeLists.txt 和 web/ 的目录）。"""
    script_dir = Path(__file__).resolve().parent
    cur = script_dir
    for _ in range(6):
        if (cur / "CMakeLists.txt").exists() and (cur / "web").is_dir():
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    # 兜底：用脚本所在目录的父目录
    return script_dir.parent


# ─── 依赖检查 ──────────────────────────────────────────────────────────────

def check_command(name: str) -> str | None:
    """检查命令是否可用，返回路径或 None。"""
    return shutil.which(name)


def ensure_node_and_pnpm() -> tuple[str, str]:
    """确保 node 和 pnpm 可用，返回 (node_path, pnpm_path)。"""
    node = check_command("node")
    if not node:
        error("未找到 node.js，请先安装 Node.js 18+")
        sys.exit(1)
    pnpm = check_command("pnpm")
    if not pnpm:
        warn("未找到 pnpm，尝试用 npm 安装...")
        npm = check_command("npm")
        if not npm:
            error("未找到 npm，无法安装 pnpm")
            sys.exit(1)
        subprocess.run([npm, "install", "-g", "pnpm"], check=True)
        pnpm = check_command("pnpm")
        if not pnpm:
            error("pnpm 安装失败")
            sys.exit(1)
    return node, pnpm


# ─── Web 构建 ──────────────────────────────────────────────────────────────

WEB_BUILD_INPUT_FILES = (
    "index.html",
    "package.json",
    "pnpm-lock.yaml",
    "pnpm-workspace.yaml",
)


def iter_web_build_inputs(web_dir: Path):
    """Yield files that can affect Vite's production output."""
    seen = set()
    for directory_name in ("src", "public"):
        directory = web_dir / directory_name
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if path.is_file() and path not in seen:
                seen.add(path)
                yield path
    for name in WEB_BUILD_INPUT_FILES:
        path = web_dir / name
        if path.is_file() and path not in seen:
            seen.add(path)
            yield path
    for pattern in ("vite.config.*", "postcss.config.*", "tailwind.config.*"):
        for path in web_dir.glob(pattern):
            if path.is_file() and path not in seen:
                seen.add(path)
                yield path


def web_build_is_stale(web_dir: Path, index_html: Path) -> bool:
    if not index_html.is_file():
        return True
    try:
        dist_mtime = index_html.stat().st_mtime
        return any(path.stat().st_mtime > dist_mtime
                   for path in iter_web_build_inputs(web_dir))
    except OSError:
        # A file changed while scanning; rebuild rather than trusting a stale dist.
        return True

def build_web(web_dir: Path, pnpm: str, force: bool = False) -> None:
    """构建 web 前端到 web/dist/。"""
    dist_dir = web_dir / "dist"
    index_html = dist_dir / "index.html"

    if not force and index_html.exists():
        if not web_build_is_stale(web_dir, index_html):
            ok("web/dist/ 已是最新，跳过构建")
            return
        info("检测到 Web 构建输入有更新，重新构建...")
    else:
        if force:
            info("强制重新构建 web...")
        else:
            info("web/dist/ 不存在，开始构建...")

    # 确保 node_modules 存在
    if not (web_dir / "node_modules").is_dir():
        info("安装 web 依赖 (pnpm install)...")
        subprocess.run([pnpm, "install"], cwd=web_dir, check=True)

    info("运行 pnpm build...")
    result = subprocess.run([pnpm, "build"], cwd=web_dir)
    if result.returncode != 0:
        error(f"pnpm build 失败 (exit code {result.returncode})")
        sys.exit(1)
    ok("web 构建完成")


# ─── Desktop 构建产物定位 ───────────────────────────────────────────────────

def find_desktop_builds(build_dir: Path) -> list[Path]:
    """查找 build 根、直接子目录及 preset/config 两层布局的产物。"""
    results: list[Path] = []
    if not build_dir.is_dir():
        return results

    search_dirs = [build_dir]
    try:
        first_level = sorted(
            (path for path in build_dir.iterdir()
             if path.is_dir() and path.suffix != ".app"),
            key=lambda path: str(path).lower(),
        )
    except OSError:
        first_level = []
    search_dirs.extend(first_level)
    for first in first_level:
        try:
            search_dirs.extend(sorted(
                (path for path in first.iterdir()
                 if path.is_dir() and path.suffix != ".app"),
                key=lambda path: str(path).lower(),
            ))
        except OSError:
            continue

    seen = set()
    for child in search_dirs:
        # macOS .app bundle
        app_bundle = child / "ACECode.app"
        if app_bundle.is_dir():
            resolved = app_bundle.resolve()
            if resolved not in seen:
                seen.add(resolved)
                results.append(app_bundle)
        # Windows .exe
        exe = child / "acecode-desktop.exe"
        if exe.is_file():
            resolved = exe.resolve()
            if resolved not in seen:
                seen.add(resolved)
                results.append(exe)
        # Linux / macOS 裸可执行文件（非 .app）
        binary = child / "acecode-desktop"
        if binary.is_file() and os.access(binary, os.X_OK):
            resolved = binary.resolve()
            if resolved not in seen:
                seen.add(resolved)
                results.append(binary)

    return results


def display_path(path: Path, project_root: Path) -> str:
    """Prefer a repo-relative path, but support explicitly external builds."""
    resolved = path.resolve()
    try:
        return str(resolved.relative_to(project_root.resolve()))
    except ValueError:
        return str(resolved)


def pick_desktop_build(builds: list[Path], preferred: str | None = None) -> Path | None:
    """从可用构建中选择一个。优先选 release，其次按名称排序。"""
    if not builds:
        return None
    if preferred:
        for b in builds:
            if preferred in str(b):
                return b
    # 优先选 desktop-release
    for b in builds:
        if "desktop-release" in str(b):
            return b
    # 其次选 release
    for b in builds:
        if "release" in str(b).lower():
            return b
    # 兜底选第一个
    return builds[0]


# ─── 启动 Desktop ───────────────────────────────────────────────────────────

def launch_desktop(desktop_path: Path, dev_web_dir: Path) -> None:
    """启动 desktop app，并设置 ACECODE_DEV_WEB_DIR 环境变量。"""
    env = os.environ.copy()
    env["ACECODE_DEV_WEB_DIR"] = str(dev_web_dir.resolve())

    info(f"ACECODE_DEV_WEB_DIR = {dev_web_dir.resolve()}")

    if sys.platform == "darwin":
        if desktop_path.suffix == ".app":
            # macOS .app bundle
            info(f"启动 {desktop_path.name}...")
            subprocess.Popen(["open", str(desktop_path)], env=env)
        else:
            # 裸可执行文件
            info(f"启动 {desktop_path.name}...")
            subprocess.Popen([str(desktop_path)], env=env)
    elif sys.platform == "win32":
        info(f"启动 {desktop_path.name}...")
        # Windows 下用 DETACHED_PROCESS 避免阻塞
        DETACHED_PROCESS = 0x00000008
        subprocess.Popen(
            [str(desktop_path)],
            env=env,
            creationflags=DETACHED_PROCESS,
            close_fds=True,
        )
    else:
        # Linux
        info(f"启动 {desktop_path.name}...")
        subprocess.Popen([str(desktop_path)], env=env, start_new_session=True)


# ─── 主流程 ─────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="ACECode Desktop 一键开发脚本 — 只改 H5 时快速验证，无需重编 C++",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s                # 自动构建(如需)并启动 desktop
  %(prog)s --rebuild      # 强制重新构建 web 再启动
  %(prog)s --no-build     # 跳过构建，直接启动
  %(prog)s --list         # 列出可用的 desktop 构建产物
  %(prog)s --build-dir build/macos-x64-desktop-release  # 指定构建目录
        """,
    )
    parser.add_argument("--rebuild", action="store_true", help="强制重新构建 web 前端")
    parser.add_argument("--no-build", action="store_true", help="跳过 web 构建，直接启动 desktop")
    parser.add_argument("--list", action="store_true", help="列出可用的 desktop 构建产物并退出")
    parser.add_argument("--build-dir", type=str, default=None, help="指定 desktop 构建目录（相对于项目根或绝对路径）")
    parser.add_argument("--root", type=str, default=None, help="指定项目根目录（自动检测失败时使用）")

    args = parser.parse_args()

    banner("ACECode Desktop Dev Launcher")

    # 1. 定位项目根目录
    if args.root:
        project_root = Path(args.root).resolve()
    else:
        project_root = find_project_root()
    if not (project_root / "CMakeLists.txt").exists():
        error(f"项目根目录无效: {project_root} (未找到 CMakeLists.txt)")
        sys.exit(1)
    info(f"项目根目录: {project_root}")

    web_dir = project_root / "web"
    build_dir = project_root / "build"
    dev_web_dir = web_dir / "dist"

    # 2. --list 模式
    if args.list:
        builds = find_desktop_builds(build_dir)
        if not builds:
            warn("未找到任何 desktop 构建产物")
            info(f"请先在 {build_dir} 下构建 desktop:")
            info("  cmake -S . -B build/<dir> -DACECODE_BUILD_DESKTOP=ON ...")
            info("  cmake --build build/<dir> --target acecode-desktop")
        else:
            ok(f"找到 {len(builds)} 个 desktop 构建产物:")
            for i, b in enumerate(builds, 1):
                print(f"  {i}. {display_path(b, project_root)}")
        return

    # 3. 检查并构建 web
    if not args.no_build:
        _, pnpm = ensure_node_and_pnpm()
        build_web(web_dir, pnpm, force=args.rebuild)
    else:
        if not (dev_web_dir / "index.html").exists():
            error(f"web/dist/index.html 不存在，无法跳过构建。请先运行 pnpm build，或去掉 --no-build")
            sys.exit(1)
        ok("跳过 web 构建 (--no-build)")

    # 4. 定位 desktop 构建产物
    if args.build_dir:
        specified = Path(args.build_dir)
        if not specified.is_absolute():
            specified = project_root / specified
        specified = specified.resolve()
        # 支持指定到 .app / .exe / 目录
        if (specified.suffix == ".app" or specified.name.endswith(".exe") or
                specified.name == "acecode-desktop"):
            desktop_path = specified
        else:
            candidates = find_desktop_builds(specified)
            if not candidates:
                error(f"在指定目录下未找到 desktop 构建产物: {specified}")
                sys.exit(1)
            else:
                desktop_path = pick_desktop_build(candidates)
    else:
        builds = find_desktop_builds(build_dir)
        if not builds:
            error("未找到任何 desktop 构建产物")
            info("请先构建 desktop:")
            info("  cmake -S . -B build/<dir> -DACECODE_BUILD_DESKTOP=ON ...")
            info("  cmake --build build/<dir> --target acecode-desktop")
            info("或使用 --list 查看可用构建")
            sys.exit(1)
        desktop_path = pick_desktop_build(builds)
        if len(builds) > 1:
            info(f"找到 {len(builds)} 个构建产物，自动选择: "
                 f"{display_path(desktop_path, project_root)}")
            info("(可用 --build-dir 指定其他构建，或 --list 查看全部)")

    if not desktop_path.exists():
        error(f"desktop 构建产物不存在: {desktop_path}")
        sys.exit(1)
    ok(f"Desktop 构建: {display_path(desktop_path, project_root)}")

    # 5. 启动 desktop
    launch_desktop(desktop_path, dev_web_dir)
    ok("Desktop 已启动")

    # 6. 打印使用提示
    print()
    print(_c("── 开发提示 ──────────────────────────────────────", "35"))
    print("  1. 修改 web/src/ 下的代码后，重新构建:")
    print(f"       cd {web_dir} && pnpm build")
    print("     或重新运行本脚本加 --rebuild")
    print("  2. 在 Desktop 窗口按 F5 刷新页面即可看到改动")
    print("  3. 按 F11 打开 WebView 开发者工具调试")
    print("  4. 无需重新编译 C++!")
    print(_c("──────────────────────────────────────────────────", "35"))
    print()


if __name__ == "__main__":
    main()
