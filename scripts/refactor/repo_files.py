#!/usr/bin/env python3
"""Tracked-file and byte-preserving primitives shared by refactor tools."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".mm", ".c", ".cc", ".hh"}
NESTED_WORKTREES = (".claude/worktrees/", ".worktrees/", ".acecode/worktrees/")


def git(root: Path, *args: str, timeout: int = 60) -> bytes:
    result = subprocess.run(
        ["git", "-c", "core.fsmonitor=false", "-C", str(root), *args],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def repo_root(path: str | Path = ".") -> Path:
    return Path(os.fsdecode(git(Path(path), "rev-parse", "--show-toplevel").strip()))


def tracked_files(root: Path, prefixes: Iterable[str] = ()) -> list[str]:
    """Never enumerate the working directory or recurse into a gitlink/worktree."""
    prefixes = tuple(prefixes)
    paths = []
    for record in git(root, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        metadata, raw_path = record.split(b"\t", 1)
        mode, _object, stage = metadata.split()
        path = os.fsdecode(raw_path).replace("\\", "/")
        if mode == b"160000" or stage != b"0":
            continue
        if path.startswith(NESTED_WORKTREES):
            continue
        if prefixes and not any(path == p.rstrip("/") or path.startswith(p.rstrip("/") + "/") for p in prefixes):
            continue
        paths.append(path)
    return sorted(set(paths))


def tracked_path(root: Path, relative: str, files: Iterable[str]) -> Path:
    """Reject writes outside this checkout, symlinks and untracked files."""
    if relative not in files:
        raise ValueError(f"not a tracked file: {relative}")
    path = root / relative
    if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"path leaves the checkout: {relative}")
    return path


def byte_lines(data: bytes) -> list[bytes]:
    # bytes.splitlines also treats uncommon control bytes as newlines; C++ only
    # needs LF/CRLF and preserving arbitrary payload bytes is intentional.
    parts = data.split(b"\n")
    return [line + b"\n" for line in parts[:-1]] + ([parts[-1]] if parts[-1] else [])


def replace_path_prefix(data: bytes, old: str, new: str) -> bytes:
    """Replace a directory prefix at a path boundary, without normalizing bytes."""
    if not old.endswith("/") or not new.endswith("/"):
        raise ValueError("directory prefixes must include a trailing slash")
    pattern = rb"(?<![A-Za-z0-9_./\\-])" + re.escape(old.encode("utf-8"))
    return re.sub(pattern, lambda _match: new.encode("utf-8"), data)


def write_bytes_if_changed(path: Path, original: bytes, updated: bytes) -> bool:
    if original == updated:
        return False
    if path.read_bytes() != original:
        raise RuntimeError(f"file changed while preparing edit: {path}")
    path.write_bytes(updated)
    return True


def emit_json(value: object, output: str | None = None) -> None:
    data = json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if output:
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        Path(output).write_bytes(data.encode("utf-8"))
    else:
        sys.stdout.buffer.write(data.encode("utf-8"))
