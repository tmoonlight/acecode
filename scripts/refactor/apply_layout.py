"""按阶段执行 src_layout_map.tsv:搬迁、include / CMake / 文档路径改写,拆成 P3 要求的 M1 / M2 / M2b / M3 提交。

P3-01 演练与 P3-02 正式搬迁用它从固定 base 生成提交;P2 阶段(如 P2-08)也能用它做机械部分。
只复用 migrate_branch / migration_* 里已经过验证的改写函数,不自己发明第二套路径规则。

  python scripts/refactor/apply_layout.py plan    --phase P3                     # 只报告:哪些文件搬到哪、更早阶段还有哪些行没做完
  python scripts/refactor/apply_layout.py move    --phase P3 [--commit]          # M1:只 git mv,0 行内容改动
  python scripts/refactor/apply_layout.py rewrite --phase P3 [--commit]          # M2:CMake / cpp_source_paths 清单 + 文档路径 + help 站点 + include 规范化
  python scripts/refactor/apply_layout.py seed    --seed-version 2026-09-29.1 [--commit]   # M2b:种子 SKILL 路径 + 版本 / 哈希 / 测试字面量
  python scripts/refactor/apply_layout.py blame   --revs SHA1 SHA2 --title "P3 M1/M2" [--commit]   # M3:登记 .git-blame-ignore-revs

规则:
- 只选 --phase 指定阶段(可重复给多个)里 kind=move 的行。更早阶段还有 old_path 存在的行时拒绝执行,
  --allow-unfinished 只供演练放行。
- 目标路径:P3 阶段用映射表的 new_path(最终布局);P2 阶段用 transition_path(new_path),
  即 src/<分组>/<模块>/ 落到 src/<模块>/,tests/ 与 cmake/ 不变。
- move 前要求没有未提交的已跟踪改动(--allow-dirty 放行),目标不存在(含未跟踪文件),不碰符号链接。
  move 之后核对暂存区只有 R100 重命名,并删掉搬空的目录。
- rewrite 在 move 之后跑:CMake 里的模块相对路径按「搬迁前的位置」解析(用本阶段映射反查),
  再换算到新位置;文档用 migration_docs.documents_plan(改了 docs/help-source 时重跑 help 构建);
  最后跑 normalize_includes。P3 的 include 改动应为 0(模块根形式在分组搬迁下不变)。
- --commit 用系列约定的提交前缀:move 在 P3 打 [no-build]、在 P2 打 [mechanical];rewrite 打 [mechanical]。

输出 JSON(--output)记录 moves / 改写文件 / include 改动行数 / 未完成行,供验证记录引用。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from hashlib import sha256
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import normalize_includes  # noqa: E402
from layout import IncludeIndex, LayoutMap, read_tsv, transition_path  # noqa: E402
from migrate_branch import docs as seed_docs, verify_destination  # noqa: E402
from migration_docs import documents_plan  # noqa: E402
from migration_git import entries  # noqa: E402
from migration_paths import build_file, managed, rewrite_build_paths  # noqa: E402
from repo_files import emit_json, tracked_path, write_bytes_if_changed  # noqa: E402

SUBJECT = "refactor20260927(layers/{phase}): {message}{tag}"
DEFAULT_MESSAGES = {
    "move": "按映射表搬迁源文件与镜像测试",
    "rewrite": "改写 CMake 清单、文档路径与 include",
    "seed": "更新种子 SKILL 路径与 bundle 版本",
    "blame": "登记机械提交到 .git-blame-ignore-revs",
}
EXTRA_BUILD_FILES = ("deepin/CMakeLists.txt",)


def git(root: Path, *args: str, check: bool = True) -> bytes:
    """按用户自己的 git 配置执行(尤其是 core.autocrlf):这里改的是用户的检出,不是隔离克隆。

    migration_git.command 固定 -c core.autocrlf=false,在 autocrlf=true 的检出里 add 会把工作区的
    CRLF 原样写进 blob;演练 M2 里 3 个 LF blob 的文件因此各多出几百个 CR。只读的索引解析仍用 migration_git。
    """
    environment = os.environ.copy()
    for key in ("GIT_INDEX_FILE", "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
        environment.pop(key, None)
    result = subprocess.run(["git", "-c", "core.longpaths=true", "-c", "submodule.recurse=false", "-C", str(root), *args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment, timeout=600, check=False)
    if check and result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace").strip() or result.stdout.decode("utf-8", "replace").strip())
    return result.stdout


def revision(root: Path, ref: str) -> str:
    return git(root, "rev-parse", "--verify", ref + "^{commit}").decode("ascii").strip()


def phase_key(phase: str) -> tuple[int, int]:
    match = re.fullmatch(r"P(\d+)(?:-(\d+))?", phase)
    return (int(match[1]), int(match[2] or 0)) if match else (99, 0)


def layout_for(phases: list[str]) -> str:
    kinds = {"final" if phase.startswith("P3") else "transition" for phase in phases}
    if len(kinds) != 1:
        raise ValueError("--phase 不能混合 P2(过渡目录)与 P3(最终布局)阶段")
    return kinds.pop()


def phase_map(rows: list[dict], phases: list[str], layout: str) -> LayoutMap:
    """只含本阶段 move 行的映射;过渡布局把 new_path 落到 src/<模块>/。"""
    selected = []
    for row in rows:
        if row["kind"] != "move" or row["phase"] not in phases:
            continue
        new = row["new_path"]
        if layout == "transition":
            new = transition_path(new)
        selected.append({**row, "new_path": new})
    return LayoutMap(selected)


def unfinished_rows(rows: list[dict], phases: list[str], files: list[str]) -> list[dict]:
    """更早阶段里 old_path 还存在的 move 行:说明前一阶段没做完,不能跳过它搬后面的。"""
    earliest = min(phase_key(phase) for phase in phases)
    pending = []
    for row in rows:
        if row["kind"] != "move" or row["phase"] in phases or phase_key(row["phase"]) >= earliest:
            continue
        old = row["old_path"]
        if any(path == old or old.endswith("/") and path.startswith(old) for path in files):
            pending.append({"phase": row["phase"], "old_path": old, "new_path": row["new_path"]})
    return pending


def plan_moves(files: list[str], mapping: LayoutMap) -> dict[str, str]:
    moves = {}
    for path in files:
        target = mapping.translate(path)
        if target and target != path:
            moves[path] = target
    return moves


def managed_files(root: Path) -> tuple[dict, list[str]]:
    inventory = entries(root)  # 未合并的索引项在这里就被拒绝
    return inventory, [path for path, entry in inventory.items() if managed(path, entry)]


def require_clean(root: Path, allow_dirty: bool) -> None:
    # 新建的 worktree 在第一次 status 前 stat 缓存未刷新,会把整批文件误报成已修改;先刷新再判断。
    git(root, "update-index", "-q", "--refresh", check=False)
    status = git(root, "status", "--porcelain", "--untracked-files=no").decode("utf-8", "replace").strip()
    if status and not allow_dirty:
        raise ValueError("工作区有未提交的已跟踪改动,先提交或加 --allow-dirty:\n" + status[:2000])


def remove_empty_dirs(root: Path, moved: list[str]) -> list[str]:
    removed = []
    for old in sorted({str(Path(path).parent) for path in moved}, key=len, reverse=True):
        directory = root / old
        while directory != root and directory.is_dir() and not any(directory.iterdir()):
            directory.rmdir()
            removed.append(directory.relative_to(root).as_posix())
            directory = directory.parent
    return removed


def step_move(root: Path, rows: list[dict], phases: list[str], *, dry_run: bool, allow_unfinished: bool, allow_dirty: bool) -> dict:
    layout = layout_for(phases)
    inventory, files = managed_files(root)
    mapping = phase_map(rows, phases, layout)
    moves = plan_moves(files, mapping)
    pending = unfinished_rows(rows, phases, files)
    report = {"schema": 1, "mode": "move", "phases": phases, "layout": layout, "moves": moves,
              "unfinished": pending, "dry_run": dry_run, "applied": False}
    if pending and not allow_unfinished:
        report["error"] = f"{len(pending)} 条更早阶段的映射行还没执行(如 {pending[0]['old_path']}),先完成它们或加 --allow-unfinished(仅演练)"
        return report
    for old, new in moves.items():
        if inventory[old].mode == "120000":
            raise ValueError("cannot move a tracked symlink: " + old)
        verify_destination(root, new, set(moves))
    if dry_run:
        return report
    require_clean(root, allow_dirty)
    for old, new in moves.items():
        (root / new).parent.mkdir(parents=True, exist_ok=True)
        git(root, "mv", "--", old, new)
    status = git(root, "diff", "--cached", "-M100%", "--name-status").decode("utf-8", "replace")
    kinds = {line.split("\t", 1)[0] for line in status.splitlines() if line.strip()}
    if kinds - {"R100"}:
        raise ValueError("搬迁后暂存区不只有 R100 重命名: " + ", ".join(sorted(kinds)))
    report["removed_directories"] = remove_empty_dirs(root, list(moves))
    report["applied"] = True
    return report


def is_build_file(path: str) -> bool:
    return build_file(path) or path in EXTRA_BUILD_FILES


def step_rewrite(root: Path, map_path: Path, rows: list[dict], phases: list[str], *, dry_run: bool, design_note: bool) -> dict:
    layout = layout_for(phases)
    inventory, files = managed_files(root)
    fingerprint = sha256(map_path.read_bytes()).hexdigest()
    stage_map = phase_map(rows, phases, layout)
    pending = unfinished_rows(rows, phases, files)
    # P3 用整张映射表换算(P2 遗漏的文档路径一并收口);P2 只换本阶段的行,免得把别的模块提前写成最终路径。
    # 演练时更早阶段还有行没做完,整张表会把文档指向不存在的路径(help 构建按路径存在性断言),退回本阶段的行。
    mapping = LayoutMap([r for r in rows if r["kind"] in ("move", "delete")]) if layout == "final" and not pending else stage_map
    # CMake 里的模块相对路径要按搬迁前的位置解析:把当前路径按本阶段映射反查得到旧清单,新旧并存。
    previous = [stage_map.translate(path, reverse=True) or path for path in files]
    index = IncludeIndex(sorted(set(files) | set(previous)), aliases=mapping)
    build_updates = {}
    for path in files:
        if not is_build_file(path):
            continue
        original = tracked_path(root, path, files).read_bytes()
        updated = rewrite_build_paths(original, path, index, mapping)
        if updated != original:
            build_updates[path] = (original, updated)
    doc_updates, detail = documents_plan(root, files, mapping, fingerprint)
    if not design_note:
        doc_updates = {p: u for p, u in doc_updates.items() if not (p.startswith("openspec/changes/") and p.endswith("/design.md"))}
    includes = normalize_includes.run(root, True)
    report = {"schema": 1, "mode": "rewrite", "phases": phases, "layout": layout, "map_sha256": fingerprint,
              "unfinished": pending, "whole_map": mapping is not stage_map,
              "build_files": sorted(build_updates), "documents": sorted(doc_updates),
              "include_changed_lines": includes["changed_lines"], "include_errors": includes["errors"],
              "help_generation": detail.get("help_generation"), "dry_run": dry_run, "applied": False}
    if dry_run:
        return report
    for path, (original, updated) in build_updates.items():
        write_bytes_if_changed(root / path, original, updated)
    for path, updated in doc_updates.items():
        original = (root / path).read_bytes()
        write_bytes_if_changed(root / path, original, updated)
    applied = normalize_includes.run(root, False)
    report["include_changed_lines"] = applied["changed_lines"]
    report["include_errors"] = applied["errors"]
    report["applied"] = True
    return report


def step_blame(root: Path, revs: list[str], title: str, *, dry_run: bool) -> dict:
    path = root / ".git-blame-ignore-revs"
    original = path.read_bytes() if path.exists() else b""
    resolved, warnings = [], []
    for rev in revs:
        full = git(root, "rev-parse", "--verify", rev + "^{commit}").decode("ascii").strip()
        subject = git(root, "log", "-1", "--format=%s", full).decode("utf-8", "replace").strip()
        if "[mechanical]" not in subject and "[no-build]" not in subject:
            warnings.append(f"{full[:12]} 的提交说明没有 [mechanical] / [no-build] 标记: {subject}")
        if full.encode("ascii") in original:
            warnings.append(f"{full[:12]} 已登记,跳过")
            continue
        resolved.append(full)
    block = ("\n# " + title + "\n" + "".join(rev + "\n" for rev in resolved)).encode("utf-8") if resolved else b""
    report = {"schema": 1, "mode": "blame", "revs": resolved, "warnings": warnings, "dry_run": dry_run, "applied": False}
    if dry_run or not resolved:
        return report
    path.write_bytes(original.rstrip(b"\n") + b"\n" + block)
    report["applied"] = True
    return report


def commit(root: Path, phase: str, message: str, tag: str) -> str:
    git(root, "add", "-u")
    subject = SUBJECT.format(phase=phase, message=message, tag=(" " + tag) if tag else "")
    git(root, "commit", "-q", "-m", subject)
    return revision(root, "HEAD")


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", choices=("plan", "move", "rewrite", "seed", "blame"))
    parser.add_argument("--phase", action="append", default=[], help="P2-08 / P3 …,可重复")
    parser.add_argument("--map", default=str(HERE / "src_layout_map.tsv"))
    parser.add_argument("--repo", default=".")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--commit", action="store_true", help="按系列约定提交本步")
    parser.add_argument("--message", help="提交说明正文,默认按步骤给")
    parser.add_argument("--allow-unfinished", action="store_true", help="更早阶段未做完也继续(仅演练)")
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--no-design-note", action="store_true", help="rewrite 不给活跃 openspec design.md 加映射指纹注释")
    parser.add_argument("--seed-version", help="seed 步骤的新 seed.version")
    parser.add_argument("--revs", nargs="*", default=[], help="blame 步骤要登记的提交")
    parser.add_argument("--title", help="blame 步骤写进忽略文件的小节标题")
    parser.add_argument("--output")
    args = parser.parse_args()

    root = Path(args.repo).resolve()
    map_path = Path(args.map).resolve()
    rows = read_tsv(map_path)
    phase_label = "+".join(args.phase) if args.phase else "P3"
    if args.step in ("plan", "move", "rewrite") and not args.phase:
        parser.error(f"{args.step} 需要 --phase")
    if args.step == "plan":
        report = step_move(root, rows, args.phase, dry_run=True, allow_unfinished=True, allow_dirty=True)
        report["mode"] = "plan"
    elif args.step == "move":
        report = step_move(root, rows, args.phase, dry_run=args.dry_run, allow_unfinished=args.allow_unfinished, allow_dirty=args.allow_dirty)
    elif args.step == "rewrite":
        report = step_rewrite(root, map_path, rows, args.phase, dry_run=args.dry_run, design_note=not args.no_design_note)
    elif args.step == "seed":
        if not args.seed_version:
            parser.error("seed 需要 --seed-version")
        report = seed_docs(root, map_path, dry_run=args.dry_run, version=args.seed_version)
    else:
        if not args.revs or not args.title:
            parser.error("blame 需要 --revs 与 --title")
        report = step_blame(root, args.revs, args.title, dry_run=args.dry_run)

    if args.commit and report.get("applied", args.step == "seed" and not args.dry_run):
        tag = {"move": "[no-build]" if report.get("layout") == "final" else "[mechanical]", "rewrite": "[mechanical]"}.get(args.step, "")
        report["commit"] = commit(root, phase_label, args.message or DEFAULT_MESSAGES[args.step], tag)
    emit_json(report, args.output)
    if report.get("error") or report.get("include_errors"):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
