#!/usr/bin/env python3
"""Read-only branch/worktree inventory, including patch-equivalent commits."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess

from repo_files import emit_json, git, repo_root


def worktrees(root: Path) -> list[dict]:
    result, entry = [], {}
    for field in git(root, "worktree", "list", "--porcelain", "-z").split(b"\0"):
        if not field:
            if entry:
                result.append(entry)
                entry = {}
            continue
        key, _, value = field.decode("utf-8", "surrogateescape").partition(" ")
        entry[key] = value or True
    if entry:
        result.append(entry)
    return result


def inspect_ref(root: Path, base: str, ref: str, timeout: int) -> dict:
    item = {"ref": ref, "error": None}
    try:
        item["head"] = git(root, "rev-parse", ref).decode().strip()
        item["ahead"] = int(git(root, "rev-list", "--count", f"{base}..{ref}"))
        item["behind"] = int(git(root, "rev-list", "--count", f"{ref}..{base}"))
        cherry = git(root, "cherry", base, ref, timeout=timeout).decode().splitlines()
        item["cherry"] = cherry
        item["unique_commits"] = sum(line.startswith("+") for line in cherry)
        item["equivalent_commits"] = sum(line.startswith("-") for line in cherry)
        # Count paths changed by unique commits, not a broad tree diff polluted by
        # upstream changes. This also works for pre-filter-repo patch equivalents.
        changed = set()
        for line in cherry:
            if line.startswith("+"):
                changed.update(p.decode("utf-8", "surrogateescape") for p in git(
                    root, "diff-tree", "--root", "--no-commit-id", "--name-only", "-r", "-m", "-z", line[2:], timeout=timeout
                ).split(b"\0") if p)
        item["src_paths"] = sorted(p for p in changed if p.startswith("src/"))
        item["test_paths"] = sorted(p for p in changed if p.startswith("tests/"))
    except (RuntimeError, subprocess.TimeoutExpired, ValueError) as error:
        item["error"] = str(error)
    return item


def inspect_worktree(entry: dict, timeout: int) -> dict:
    item = dict(entry)
    try:
        status = git(Path(str(entry["worktree"])), "status", "--porcelain=v1", "-z", "--untracked-files=normal", "--ignore-submodules=none", timeout=timeout)
        # Keep NUL-separated paths intact: renames have two fields, and filenames
        # may contain whitespace/newlines. Dirty is never inferred from an error.
        item["dirty"] = bool(status)
        item["status_records"] = [x.decode("utf-8", "surrogateescape") for x in status.split(b"\0") if x]
        item["status_error"] = None
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        item["dirty"] = None
        item["status_records"] = []
        item["status_error"] = str(error)
    return item


def inventory(root: Path, base: str, timeout: int = 60, jobs: int = 4, include_remotes: bool = True) -> dict:
    base_sha = git(root, "rev-parse", "--verify", base + "^{commit}").decode().strip()
    namespaces = ("refs/heads/", "refs/remotes/") if include_remotes else ("refs/heads/",)
    refs = set()
    for row in git(root, "for-each-ref", "--format=%(refname)\t%(symref)", *namespaces).decode().splitlines():
        ref, _, symbolic_target = row.partition("\t")
        if not symbolic_target and not ref.endswith("/HEAD"):
            refs.add(ref)
    trees = worktrees(root)
    refs.update(str(tree.get("branch", tree["HEAD"])) for tree in trees if "HEAD" in tree)
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        branches = list(executor.map(lambda ref: inspect_ref(root, base_sha, ref, timeout), sorted(refs)))
        trees = list(executor.map(lambda tree: inspect_worktree(tree, timeout), trees))
    return {"schema": 1, "base": base, "base_sha": base_sha, "branches": branches, "worktrees": trees}


def markdown(report: dict) -> str:
    def cell(value: object) -> str:
        return str(value).replace("|", "\\|").replace("\n", "<br>")
    rows = ["# Branch inventory", "", f"Base: `{report['base']}` (`{report['base_sha']}`). Read-only snapshot.", "",
            "`src` / `tests` count distinct paths changed by commits marked `+` by `git cherry`.", "",
            "| Ref | Ahead | Behind | Cherry + | Cherry - | src | tests | Error |",
            "|---|---:|---:|---:|---:|---:|---:|---|"]
    for branch in report["branches"]:
        values = [branch["ref"], branch.get("ahead", "?"), branch.get("behind", "?"), branch.get("unique_commits", "?"), branch.get("equivalent_commits", "?"), len(branch.get("src_paths", [])), len(branch.get("test_paths", [])), branch["error"] or ""]
        rows.append("| " + " | ".join(cell(v) for v in values) + " |")
    rows += ["", "| Worktree | Ref / HEAD | Dirty | Status error |", "|---|---|---|---|"]
    for tree in report["worktrees"]:
        rows.append("| " + " | ".join(cell(v) for v in [tree["worktree"], tree.get("branch", tree.get("HEAD", "?")), {True: "yes", False: "no", None: "unknown"}[tree["dirty"]], tree["status_error"] or ""]) + " |")
    rows += ["", "## git cherry results and unique paths", ""]
    for branch in report["branches"]:
        if branch.get("cherry") or branch["error"]:
            rows += [f"### {branch['ref']}", "", "```text", *branch.get("cherry", []), "```", ""]
            rows += [f"- `{p}`" for p in branch.get("src_paths", []) + branch.get("test_paths", [])]
            rows.append("")
    return "\n".join(rows) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--base", default="master")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument("--output")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--local-only", action="store_true", help="omit remote-tracking refs (included by default; never fetch)")
    args = parser.parse_args()
    report = inventory(repo_root(args.repo), args.base, args.timeout, args.jobs, not args.local_only)
    if args.format == "json":
        emit_json(report, args.output)
    else:
        data = markdown(report)
        if args.output:
            Path(args.output).write_bytes(data.encode("utf-8"))
        else:
            print(data, end="")
    return int(any(b["error"] for b in report["branches"]) or any(t["status_error"] for t in report["worktrees"]))


if __name__ == "__main__":
    raise SystemExit(main())
