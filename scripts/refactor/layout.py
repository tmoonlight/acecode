"""Shared, deterministic TSV policy and include resolution (no tree walking)."""
from __future__ import annotations

import csv
from dataclasses import dataclass
from fnmatch import fnmatchcase
from pathlib import Path, PurePosixPath
import posixpath
import re

GROUPS = ("base", "domain", "adapters", "engine", "host", "apps")
INCLUDE = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\r\n]+)[>"]', re.MULTILINE)


def read_tsv(path: Path) -> list[dict[str, str]]:
    lines = (line for line in path.read_text(encoding="utf-8").splitlines() if line and not line.startswith("#"))
    return list(csv.DictReader(lines, delimiter="\t"))


def mask_cpp(data: bytes, strings: bool = True) -> bytes:
    """Mask comments/literals while preserving offsets and line numbers.

    Raw strings may contain quotes, comment delimiters and entire C++ examples.
    Preprocessor branches deliberately remain: every platform must be checked.
    """
    pattern = re.compile(rb'R"([^ ()\\\t\r\n]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\r\n]*|/\*.*?\*/', re.DOTALL)
    def replace(match):
        value = match.group()
        if not strings and not value.startswith((b"//", b"/*", b'R"')):
            return value
        return re.sub(rb"[^\r\n]", b" ", value)
    return pattern.sub(replace, data)


@dataclass(frozen=True)
class Module:
    prefix: str
    name: str
    group: str
    rank: int


class LayoutMap:
    def __init__(self, rows: list[dict[str, str]]):
        self.rows = rows
        self.moves = sorted((r for r in rows if r["kind"] in ("move", "delete")), key=lambda r: len(r["old_path"]), reverse=True)

    def translate(self, path: str, reverse: bool = False) -> str | None:
        source, target = ("new_path", "old_path") if reverse else ("old_path", "new_path")
        rows = sorted(self.moves, key=lambda r: len(r[source]), reverse=True)
        for row in rows:
            old, new = row[source], row[target]
            if old == "-":
                continue
            if path == old or old.endswith("/") and path.startswith(old):
                return None if new == "-" else new + path[len(old):]
        return path


class LayerPolicy:
    def __init__(self, rows: list[dict[str, str]], mapping: LayoutMap):
        self.rows, self.mapping = rows, mapping
        self.modules = [Module(r["path"], r["module"], r["group"], int(r["rank"])) for r in rows if r["kind"] == "module"]
        self.by_name = {m.name: m for m in self.modules}

    def canonical(self, path: str, transition: bool = True) -> str:
        if any(path.startswith(f"src/{group}/") for group in GROUPS):
            return path
        # P2 creates modules at src/<module> before their P3 group move.
        prefix = path.split("/")
        if transition and len(prefix) >= 3 and prefix[0] == "src" and prefix[1] in self.by_name:
            translated = self.mapping.translate(path)
            if translated and translated != path:
                return translated
            return self.by_name[prefix[1]].prefix + "/".join(prefix[2:])
        return (self.mapping.translate(path) or path) if transition else path

    def module_for(self, path: str, transition: bool = True) -> Module | None:
        canonical = self.canonical(path, transition)
        if canonical.startswith("tests/"):
            parts = canonical.split("/")
            if len(parts) > 2:
                name = parts[2] if parts[1] == "test_support" and len(parts) > 3 else parts[1]
                return self.by_name.get(name)
        return next((module for module in self.modules if canonical.startswith(module.prefix)), None)

    def allowed(self, rule: str, path: str) -> bool:
        return any(r["kind"] == "allow" and r["rule"] == rule and fnmatchcase(path, r["path"]) for r in self.rows)


def load_policy(root: Path, policy: str = "src/layers.tsv", mapping: str = "scripts/refactor/src_layout_map.tsv") -> LayerPolicy:
    return LayerPolicy(read_tsv(root / policy), LayoutMap(read_tsv(root / mapping)))


class IncludeIndex:
    def __init__(self, files: list[str], generated: tuple[str, ...] = ("version.hpp",), aliases: LayoutMap | None = None):
        self.files = set(files)
        self.roots = ["src", *(f"src/{g}" for g in GROUPS), "tests", "external/stb"]
        self.generated = generated
        self.aliases = aliases

    def resolve(self, includer: str, name: str) -> list[str]:
        roots = [str(PurePosixPath(includer).parent), *self.roots]
        candidates = {posixpath.normpath(f"{root}/{name}") for root in roots}
        found = candidates & self.files
        if not found and self.aliases:
            found = {self.aliases.translate(candidate) for candidate in candidates} & self.files
        if name in self.generated:
            found.add("@generated/" + name)
        return sorted(found)

    def canonical_include(self, target: str) -> str:
        for group in GROUPS:
            if target.startswith(f"src/{group}/"):
                return target[len(group) + 5:]
        for prefix in ("src/", "tests/", "external/", "@generated/"):
            if target.startswith(prefix):
                return target[len(prefix):]
        return target


def include_matches(data: bytes):
    # Comments are ignored without changing include positions in the original.
    return INCLUDE.finditer(mask_cpp(data, strings=False))
