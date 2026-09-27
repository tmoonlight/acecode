#!/usr/bin/env python3
"""Textual layer guards R1-R14, including inactive platform preprocessor arms."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import date
from fnmatch import fnmatchcase
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "refactor"))
from layout import GROUPS, IncludeIndex, LayerPolicy, include_matches, load_policy, mask_cpp
from repo_files import SOURCE_SUFFIXES, emit_json, repo_root, tracked_files
from check_file_size import inspect as inspect_sizes, read_baseline

STANDARD_HEADERS = set("algorithm any array atomic bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes ciso646 climits clocale cmath codecvt complex condition_variable csetjmp csignal cstdarg cstdbool cstddef cstdint cstdio cstdlib cstring ctgmath ctime cuchar cwchar cwctype deque exception execution filesystem forward_list fstream functional future initializer_list iomanip ios iosfwd iostream istream iterator limits list locale map memory memory_resource mutex new numeric optional ostream queue random ratio regex scoped_allocator set shared_mutex sstream stack stdexcept streambuf string string_view strstream system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector assert.h ctype.h errno.h float.h limits.h locale.h math.h setjmp.h signal.h stdarg.h stdbool.h stddef.h stdint.h stdio.h stdlib.h string.h time.h wchar.h wctype.h".split())


def inspect(root: Path, files: list[str], policy: LayerPolicy, transition: bool = True, strict: bool = False, today: date | None = None) -> dict:
    today = today or date.today()
    findings, suppressed = [], []
    index = IncludeIndex(files)
    exceptions = []

    def add(rule: str, path: str, line: int, message: str, target: str = ""):
        canonical = policy.canonical(path, transition)
        for exception in exceptions:
            if rule in exception["rule"].split(",") and fnmatchcase(canonical, exception["path"]) and fnmatchcase(target, exception["target"]):
                suppressed.append({"rule": rule, "file": path, "line": line, "message": message, "owner": exception["owner"]})
                return
        findings.append({"rule": rule, "file": path, "line": line, "message": message, "target": target})

    for row in policy.rows:
        if row["kind"] != "exception":
            continue
        try:
            if not all(row.get(key) for key in ("path", "rule", "target", "owner", "expires", "note")):
                raise ValueError("missing from/to/rule/owner/expiry/reason")
            expires = date.fromisoformat(row["expires"])
            if expires <= today:
                raise ValueError("exception expired on " + row["expires"])
            if strict:
                raise ValueError("strict mode requires an empty exceptions table")
            exceptions.append(row)
        except ValueError as error:
            add("R13", "src/layers.tsv", 1, str(error), row.get("target", ""))

    names = [m.name for m in policy.modules]
    for name, count in Counter(names).items():
        if count > 1:
            add("R9", "src/layers.tsv", 1, f"module {name} is registered {count} times")
    for module in policy.modules:
        if module.group not in GROUPS or module.prefix != f"src/{module.group}/{module.name}/":
            add("R9", "src/layers.tsv", 1, "noncanonical module prefix " + module.prefix)

    include_edges = 0
    source_files = [p for p in files if p.startswith(("src/", "tests/")) and Path(p).suffix in SOURCE_SUFFIXES and "/stb/" not in p]
    for path in source_files:
        canonical = policy.canonical(path, transition)
        source = policy.module_for(path, transition)
        is_test = path.startswith("tests/")
        data = (root / path).read_bytes()
        code = mask_cpp(data)
        if source is None and not (is_test and path.startswith("tests/test_support/")):
            add("R10", path, 1, "file has no registered module")
        if path.startswith("src/") and Path(path).name == "version.hpp":
            add("R9", path, 1, "version.hpp is reserved for the generated header")
        parts = canonical.split("/")
        if path.startswith("src/") and len(parts) >= 4 and parts[1] in GROUPS:
            for subdir in parts[3:-1]:
                if subdir in names or subdir in GROUPS:
                    add("R9", path, 1, f"subdirectory {subdir} shadows a module/group")
        if is_test:
            if path.endswith((".hpp", ".h")) and not path.startswith("tests/test_support/"):
                add("R14", path, 1, "test helper headers must live under tests/test_support/")
            if len(parts) < 3 or parts[1] not in names and parts[1] != "test_support":
                add("R14", path, 1, "test directory must mirror a registered module")

        for rule in (r for r in policy.rows if r["kind"] == "symbol"):
            if is_test or not fnmatchcase(canonical, rule["path"]) or policy.allowed(rule["rule"], canonical):
                continue
            for match in re.finditer(rule["target"].encode(), code):
                add(rule["rule"].split(":")[0], path, code.count(b"\n", 0, match.start()) + 1, "unregistered single outlet: " + rule["rule"], rule["rule"])

        for match in include_matches(data):
            name = match[2].decode("utf-8", "replace")
            line = data.count(b"\n", 0, match.start()) + 1
            quoted = match[1] == b'"'
            targets = index.resolve(path, name)
            project_targets = [t for t in targets if t.startswith(("src/", "tests/", "@generated/"))]
            project_looking = "/" in name and name.split("/")[0] in set(names) | set(GROUPS) | {"test_support", "src", "tests"}
            third_party = any(re.search(r["target"], name) for r in policy.rows if r["kind"] == "thirdparty") and not project_targets
            if ".." in name.split("/"):
                add("R8", path, line, "parent-relative include is forbidden", name)
            if name.split("/")[0] in GROUPS:
                add("R8", path, line, "include must omit the group prefix", name)
            if project_targets and not quoted:
                add("R8", path, line, "project header must use quotes", name)
            if len(project_targets) == 1 and quoted and "/" not in name and not project_targets[0].startswith("@generated/") and Path(project_targets[0]).parent != Path(path).parent:
                add("R8", path, line, "bare project include must resolve in the containing directory", name)
            if len(targets) > 1:
                add("R8", path, line, "ambiguous include: " + ", ".join(targets), name)
            if not targets and not third_party and (project_looking or quoted and ("../" in name or name.endswith((".hpp", ".h")))):
                add("R8", path, line, "unresolved project include", name)
            if len(project_targets) == 1 and quoted and "/" in name and ".." not in name.split("/"):
                expected = index.canonical_include(project_targets[0])
                if name != expected:
                    add("R8", path, line, "expected module-root include " + expected, name)

            for rule in (r for r in policy.rows if r["kind"] == "thirdparty"):
                if not project_targets and re.search(rule["target"], name) and not is_test and (source is None or source.name not in rule["note"].split(";")):
                    add("R7", path, line, "third-party dependency outside allowed modules: " + rule["note"], name)
            if source and source.group == "domain" and name.startswith("cpr/"):
                add("R3", path, line, "domain cannot depend on CPR", name)
            for restriction in (r for r in policy.rows if r["kind"] == "restrict" and fnmatchcase(canonical, r["path"])):
                if restriction["target"] == "standard" and (quoted or name not in STANDARD_HEADERS):
                    add("R6", path, line, "config/vocab may include only the standard library", name)
                if restriction["target"] == "no-ftxui" and name.startswith("ftxui/"):
                    add("R6", path, line, "TUI model must not depend on FTXUI", name)

            for target_path in project_targets:
                target_canonical = policy.canonical(target_path, transition)
                target = policy.module_for(target_path, transition)
                if not target or not source or is_test:
                    continue
                include_edges += 1
                if source.name != target.name:
                    if GROUPS.index(target.group) > GROUPS.index(source.group):
                        add("R1", path, line, f"upward group dependency {source.group} -> {target.group}", target_canonical)
                    elif target.rank >= source.rank:
                        add("R2", path, line, f"upward rank dependency {source.name}({source.rank}) -> {target.name}({target.rank})", target_canonical)
                    if source.group == "apps" and target.group == "apps":
                        allowed = {"daemon": {"web"}, "cli": {"tui", "daemon", "headless", "web"}}.get(source.name, set())
                        if target.name not in allowed:
                            add("R5", path, line, f"apps peer dependency {source.name} -> {target.name}", target_canonical)
                for rule in (r for r in policy.rows if r["kind"] == "forbid"):
                    if fnmatchcase(canonical, rule["path"]) and rule["target"] == target.name:
                        add("R3", path, line, "semantic dependency prohibition: " + target.name, target_canonical)
                if target.name == "pa" and source.name != "pa" and not policy.allowed("R11:pa", canonical):
                    add("R3", path, line, "PA include outside the contact table", target_canonical)
                if target.name == "tool_preamble" and source.name != target.name and not policy.allowed("R3:tool_preamble", canonical):
                    add("R3", path, line, "tool_preamble is private to agent/progress", target_canonical)
                for restriction in (r for r in policy.rows if r["kind"] == "restrict" and fnmatchcase(canonical, r["path"])):
                    if restriction["target"] == "base" and target.group != "base" and target.name != source.name:
                        add("R6", path, line, "desktop may only depend on base", target_canonical)
                    if restriction["target"] == "computer-helper" and target.group != "base" and not policy.allowed("R6:computer-contract", target_canonical):
                        add("R6", path, line, "computer helper may only use base and its contract headers", target_canonical)

    for path in files:
        if not path.startswith("src/") or path == "src/layers.tsv":
            continue
        parts = path.split("/")
        if len(parts) == 2 or len(parts) == 3 and parts[1] in GROUPS:
            add("R9", path, 1, "files must be inside a module, not directly in src/group")
        if not transition and len(parts) > 2 and parts[1] not in GROUPS:
            add("R10", path, 1, "legacy source root reintroduced after the freeze")
    sizes = inspect_sizes(root, files, policy, read_baseline(root / "scripts/layers/size_baseline.txt"))
    findings.extend(sizes["findings"])
    counts = {f"R{n}": 0 for n in range(1, 15)}
    counts.update(Counter(f["rule"] for f in findings))
    return {"schema": 1, "layout": "transition" if transition else "final", "source_files": len(source_files), "include_edges": include_edges, "counts": counts, "total": len(findings), "findings": sorted(findings, key=lambda f: (f["file"], f["line"], f["rule"])), "exceptions_used": suppressed, "reserved_rules": {"R4": "Not defined by the approved design; no invented check"}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--layout", choices=("auto", "transition", "final"), default="auto")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--output")
    parser.add_argument("--enforce", default="", help="comma-separated rules that block even in report mode, e.g. R8")
    parser.add_argument("--enforce-parent-includes", action="store_true", help="P1 gate: block ../ without enabling all later R8 gates")
    args = parser.parse_args()
    root = repo_root(args.repo)
    files = tracked_files(root)
    transition = args.layout == "transition" or args.layout == "auto" and not any(p.startswith(tuple(f"src/{g}/" for g in GROUPS)) for p in files)
    report = inspect(root, files, load_policy(root), transition, args.strict)
    emit_json(report, args.output)
    parent_failure = args.enforce_parent_includes and any(f["rule"] == "R8" and f["message"] == "parent-relative include is forbidden" for f in report["findings"])
    return int(args.strict and report["total"] > 0 or parent_failure or any(report["counts"].get(rule, 0) for rule in args.enforce.split(",")))


if __name__ == "__main__":
    raise SystemExit(main())
