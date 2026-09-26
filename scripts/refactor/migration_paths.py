"""Byte-preserving migration plans; no filesystem enumeration or implicit deletes."""
from __future__ import annotations

from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path, PurePosixPath
import posixpath
import re

from layout import IncludeIndex, LayoutMap, include_matches
from repo_files import NESTED_WORKTREES, SOURCE_SUFFIXES


PATH_TOKEN = re.compile(rb'(?<![A-Za-z0-9_./\\-])(?P<prefix>\$\{(?:CMAKE_SOURCE_DIR|PROJECT_SOURCE_DIR|ACECODE_SOURCE_DIR)\}/|\./)?(?P<path>(?:src|tests)/[A-Za-z0-9_./{}*?,+\x80-\xff-]+)')
RELATIVE_TOKEN = re.compile(rb'(?<![A-Za-z0-9_./\\-])(?:\.\./)+(?:src|tests)/[A-Za-z0-9_./{}*?,+\x80-\xff-]+')
CMAKE_TOKEN = re.compile(rb'(?<![A-Za-z0-9_./\\$-])(?P<prefix>\$\{CMAKE_CURRENT_SOURCE_DIR\}/)?(?P<path>[A-Za-z0-9_./+\x80-\xff-]+\.(?:cpp|hpp|h|mm|c|cc|hh|in))(?=$|[^A-Za-z0-9_./\\-])')
ROOT_DOCS = {"CLAUDE.md", "ARCHITECTURE.md", "AGENTS.md", "AGENT.md", "README.md", "README_CN.md", "tests/README.md"}
GENERATED_HELP = {"docs/help-source/sources.json", "docs/help-source/images.json", "docs/help-source/image-plan.md", "docs/help/assets/search-index.js"}


@dataclass(frozen=True)
class Entry:
    mode: str
    oid: str


def safe_relative(path: str) -> bool:
    return bool(path) and not (path.startswith(("/", "-")) or ":" in path or "\\" in path or "\0" in path or any(p in ("..", ".git") for p in PurePosixPath(path).parts))


def managed(path: str, entry: Entry) -> bool:
    return entry.mode != "160000" and not path.startswith(NESTED_WORKTREES)


def source_file(path: str) -> bool:
    return path.startswith(("src/", "tests/")) and Path(path).suffix in SOURCE_SUFFIXES and "/stb/" not in path


def build_file(path: str) -> bool:
    return path in ("CMakeLists.txt", "tests/CMakeLists.txt", "tests/cpp_source_paths.json") or path.startswith("cmake/") and (Path(path).suffix == ".cmake" or Path(path).name == "CMakeLists.txt")


def generated_help(path: str) -> bool:
    return path in GENERATED_HELP or path.startswith("docs/help/") and Path(path).suffix == ".html"


def authored_doc(path: str) -> bool:
    return path in ROOT_DOCS or path.startswith("docs/") and not generated_help(path) and Path(path).suffix in {".md", ".py", ".json", ".html", ".js", ".txt", ".rst"}


def rewrite_paths(data: bytes, mapping: LayoutMap, context: str | None = None) -> bytes:
    """One pass, longest full path/prefix first. Never match inside web/src/."""
    def replace(match):
        token = match["path"].rstrip(b".,")
        path = token.decode("utf-8", "surrogateescape")
        if match["prefix"] == b"./" and context and str(PurePosixPath(context).parent) != ".":
            return match.group()  # ./src under docs is not the repository src.
        target = mapping.translate(path)
        if target == path and not path.endswith("/"):
            directory = mapping.translate(path + "/")
            if directory and directory != path + "/":
                target = directory.rstrip("/")
        # Deleted or split symbols need human porting; never erase their text.
        if target is None or target == path:
            return match.group()
        return (match["prefix"] or b"") + target.encode("utf-8", "surrogateescape") + match["path"][len(token):]
    result = PATH_TOKEN.sub(replace, data)
    if context:
        directory = str(PurePosixPath(context).parent)
        def relative(match):
            token = match.group().rstrip(b".,")
            path = posixpath.normpath(directory + "/" + token.decode("utf-8", "surrogateescape"))
            target = mapping.translate(path)
            if target is None or target == path:
                return match.group()
            return posixpath.relpath(target, directory).encode("utf-8", "surrogateescape") + match.group()[len(token):]
        result = RELATIVE_TOKEN.sub(relative, result)
    return result


def rewrite_build_paths(data: bytes, path: str, index: IncludeIndex, mapping: LayoutMap) -> bytes:
    updated = rewrite_paths(data, mapping, path)
    # CMAKE_CURRENT_SOURCE_DIR belongs to the caller for included .cmake files;
    # only a CMakeLists.txt establishes a source-directory context we can prove.
    if PurePosixPath(path).name != "CMakeLists.txt":
        return updated
    directory = str(PurePosixPath(path).parent)
    def replace(match):
        token = match["path"].decode("utf-8", "surrogateescape")
        source = posixpath.normpath(directory + "/" + token)
        if source not in index.files:
            return match.group()
        target = mapping.translate(source)
        if target is None or target == source:
            return match.group()
        relative = posixpath.relpath(target, directory)
        return (match["prefix"] or b"") + relative.encode("utf-8", "surrogateescape")
    return CMAKE_TOKEN.sub(replace, updated)


def rewrite_includes(data: bytes, path: str, target_path: str, index: IncludeIndex, mapping: LayoutMap) -> tuple[bytes, list[dict]]:
    edits, issues = [], []
    for match in include_matches(data):
        name = match[2].decode("utf-8", "surrogateescape")
        targets = index.resolve(path, name)
        if len(targets) != 1:
            if targets or name.startswith("../"):
                issues.append({"kind": "include", "file": path, "line": data.count(b"\n", 0, match.start()) + 1, "include": name, "targets": targets, "reason": "include must resolve uniquely before mechanical migration"})
            continue
        old = targets[0]
        if old.startswith("@generated/"):
            continue
        target = mapping.translate(old)
        if target is None:
            issues.append({"kind": "deleted_include", "file": path, "include": name, "reason": "include targets a deleted source"})
            continue
        if "/" not in name and PurePosixPath(target).parent == PurePosixPath(target_path).parent and not target.startswith("tests/test_support/"):
            continue
        new_name = index.canonical_include(target)
        if new_name != name:
            edits.append((match.start(2), match.end(2), new_name.encode("utf-8", "surrogateescape")))
    for start, end, replacement in reversed(edits):
        data = data[:start] + replacement + data[end:]
    return data, issues


def semantic_issues(paths: list[str], mapping: LayoutMap) -> list[dict]:
    issues = []
    for path in paths:
        if mapping.translate(path) is None:
            issues.append({"kind": "deleted_source", "file": path, "reason": "upstream deletion cannot discard or resurrect legacy work automatically"})
        for row in mapping.rows:
            if row["kind"] == "extract" and (path == row["old_path"] or row["old_path"].endswith("/") and path.startswith(row["old_path"])):
                issues.append({"kind": "semantic_extract", "file": path, "target": row["new_path"], "reason": row.get("note", "semantic extraction requires review")})
    return issues


def transform(data: bytes, path: str, index: IncludeIndex, mapping: LayoutMap) -> tuple[bytes, list[dict]]:
    target = mapping.translate(path) or path
    issues = []
    if source_file(path):
        data, issues = rewrite_includes(data, path, target, index, mapping)
    if build_file(path):
        data = rewrite_build_paths(data, path, index, mapping)
    return data, issues


def canonical_hash(data: bytes) -> str:
    # The packaged seed C++ test hashes LF bytes, including on Windows.
    return sha256(data.replace(b"\r\n", b"\n")).hexdigest()
