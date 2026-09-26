#!/usr/bin/env python3
"""R15: lexical ownership inventory; ambiguous callback lifetime is reported."""
from __future__ import annotations

import argparse
from collections import Counter
from fnmatch import fnmatchcase
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "refactor"))
from layout import load_policy, mask_cpp
from repo_files import SOURCE_SUFFIXES, emit_json, repo_root, tracked_files

METRICS = ("raw_new", "raw_delete", "detach", "std_thread", "unsafe_capture", "delayed_injection", "raw_handle")
PATTERNS = {
    "raw_new": rb"\bnew\b(?!\s*\[)",
    "raw_delete": rb"\bdelete\b",
    "detach": rb"(?:\.|->)\s*detach\s*\(",
    "std_thread": rb"\bstd::thread\b(?!\s*::)",
    "unsafe_capture": rb"\[(?:[^\]\n]*\bthis\b[^\]\n]*|\s*&\s*(?:,[^\]\n]*)?)\]\s*(?:\([^;]*?\))?\s*(?:mutable\s*)?(?:noexcept\s*)?(?:->[^\{]+)?\{",
    "delayed_injection": rb"\bset_\w+\s*\(\s*(?:(?:const|volatile)\s+)?[\w:<>]+(?:\s+(?:const|volatile))?\s*\*\s*\w*",
    "raw_handle": rb"\b(?:sqlite3\s*\*\s*\w+|void\s*\*\s*\w*(?:handle|process|job|token|thread)\w*)\b",
}


def named_scopes(code: bytes) -> list[tuple[int, int, str]]:
    """Find named lexical scopes; lambda/control braces inherit their owner."""
    stack, scopes = [], []
    boundary = 0
    for token in re.finditer(rb"[{};]", code):
        if token.group() == b"{":
            prefix = code[boundary:token.start()]
            declaration = re.search(rb"\b(?:namespace|class|struct)\s+([\w:]+)[^;{}]*$", prefix)
            function = re.search(rb"\b([\w:]+)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept(?:\([^)]*\))?\s*)?$", prefix)
            name = (declaration[1] if declaration else function[1] if function else b"").decode()
            if name in ("if", "for", "while", "switch", "catch"):
                name = ""
            stack.append((token.start(), name))
        elif token.group() == b"}" and stack:
            start, name = stack.pop()
            if name:
                scopes.append((start, token.end(), name))
        boundary = token.end()
    return scopes


def synchronous_capture(code: bytes, match, scopes: list[tuple[int, int, str]]) -> str | None:
    prefix = code[max(0, match.start() - 600):match.start()]
    prefix = prefix[max(prefix.rfind(b";"), prefix.rfind(b"}")) + 1:]
    if re.search(rb"\b(?:wait|wait_for|wait_until)\s*\([^;{}]*,\s*$", prefix):
        return "synchronous condition-variable predicate"
    if re.search(rb"\bstd::(?:sort|stable_sort|find_if|find_if_not|remove_if|erase_if|for_each|all_of|any_of|none_of|count_if|transform|visit|lower_bound|upper_bound|partition)\s*\([^;{}]*,\s*$", prefix):
        return "synchronous standard-library algorithm"
    opening = match.end() - 1
    depth, closing = 1, None
    for token in re.finditer(rb"[{}]", code[opening + 1:]):
        depth += 1 if token.group() == b"{" else -1
        if depth == 0:
            closing = opening + 1 + token.end()
            break
    if closing is None:
        return None
    if re.match(rb"\s*\(", code[closing:]):
        return "immediately invoked lambda"
    local = re.search(rb"\b(?:const\s+)?auto\s+(\w+)\s*=\s*$", prefix)
    if local:
        # A local closure is provably synchronous only if every subsequent use in
        # its enclosing named scope is a direct call; passing/returning/capturing
        # it keeps it in the conservative escape inventory.
        ends = [end for start, end, _name in scopes if start < match.start() < end]
        if ends:
            following = code[closing:min(ends)]
            uses = list(re.finditer(rb"\b" + re.escape(local[1]) + rb"\b", following))
            if all(re.match(rb"\s*\(", following[use.end():]) for use in uses):
                return "local closure only directly invoked in its owning scope"
    return None


def scan(data: bytes, path: str, synchronous: list[dict] | None = None) -> list[dict]:
    code = mask_cpp(data)
    scopes = named_scopes(code)
    occurrences = []
    for metric, expression in PATTERNS.items():
        for match in re.finditer(expression, code, re.IGNORECASE if metric == "raw_handle" else 0):
            before = code[max(0, match.start() - 100):match.start()]
            if metric in ("raw_new", "raw_delete") and re.search(rb"\boperator\s*$", before):
                continue
            if metric == "raw_delete" and re.search(rb"=\s*$", before):
                continue  # Deleted special member functions do not own memory.
            if metric == "std_thread" and re.match(rb"\s*[&*]", code[match.end():]):
                continue  # A borrowed thread parameter/pointer is not an owner.
            line = data.count(b"\n", 0, match.start()) + 1
            item = {"file": path, "line": line, "metric": metric, "text": data[match.start():match.end()].decode("utf-8", "replace")}
            item["scopes"] = [name for start, end, name in sorted(scopes) if start < match.start() < end]
            if metric == "unsafe_capture":
                reason = synchronous_capture(code, match, scopes)
                if reason:
                    if synchronous is not None:
                        synchronous.append({**item, "reason": reason})
                    continue
                context = code[max(0, match.start() - 600):match.start()]
                context = context[max(context.rfind(b";"), context.rfind(b"}")) + 1:]
                escape = re.search(rb"(?:\bstd::(?:thread|async)|(?:subscribe|enqueue|register|callback|listener|on_\w+|set_\w+)|\.\w+\s*=)", context)
                item["lifetime"] = "escaping-context" if escape else "requires-review"
                # Unknown lifetime stays in the ratchet. The checker never assumes
                # a stored local lambda is synchronous merely because it is local.
            occurrences.append(item)
    return sorted(occurrences, key=lambda r: (r["line"], r["metric"]))


def apply_allowances(occurrences: list[dict], policy) -> tuple[list[dict], list[dict]]:
    remaining, allowed = [], []
    usage = Counter()
    for occurrence in occurrences:
        canonical = policy.canonical(occurrence["file"])
        for index, rule in enumerate(policy.rows):
            if rule["kind"] != "ownership_allow" or rule["path"] != canonical or rule["rule"] != "R15:" + occurrence["metric"]:
                continue
            if not rule["owner"] or not rule["note"] or rule["target"] not in occurrence["scopes"]:
                continue
            maximum = int(rule["rank"])
            if usage[index] >= maximum:
                continue
            usage[index] += 1
            allowed.append({**occurrence, "reason": rule["note"], "owner": rule["owner"]})
            break
        else:
            remaining.append(occurrence)
    return remaining, allowed


def inspect(root: Path, baseline: dict | None = None, final: bool = False) -> dict:
    policy = load_policy(root)
    occurrences, exemptions, allowed_occurrences, synchronous = [], [], [], []
    counts = {}
    for path in tracked_files(root, ("src",)):
        if Path(path).suffix not in SOURCE_SUFFIXES:
            continue
        canonical = policy.canonical(path)
        exempt = next((r for r in policy.rows if r["kind"] == "exempt" and "R15" in r["rule"].split(",") and (fnmatchcase(canonical, r["path"]) or fnmatchcase(path, r["path"]))), None)
        if exempt:
            exemptions.append({"file": path, "reason": exempt["note"]})
            continue
        found = scan((root / path).read_bytes(), path, synchronous)
        occurrences.extend(found)
        remaining, allowed = apply_allowances(found, policy)
        allowed_occurrences.extend(allowed)
        count = Counter(item["metric"] for item in remaining)
        if count:
            counts[canonical] = dict(count)
    totals = {metric: sum(v.get(metric, 0) for v in counts.values()) for metric in METRICS}
    findings = []
    if baseline is not None:
        for path, values in counts.items():
            for metric, count in values.items():
                maximum = baseline.get("files", {}).get(path, {}).get(metric, 0)
                if count > maximum:
                    findings.append({"rule": "R15", "file": path, "metric": metric, "count": count, "allowed": maximum})
    if final:
        for path, values in counts.items():
            for metric, count in values.items():
                prohibited = metric in ("raw_new", "raw_delete", "detach", "std_thread", "raw_handle")
                prohibited |= metric == "unsafe_capture" and path.startswith(("src/engine/agent/", "src/apps/tui/app/", "src/host/session_host/"))
                prohibited |= metric == "delayed_injection" and path.startswith("src/engine/agent/")
                if prohibited and count:
                    findings.append({"rule": "R15-final", "file": path, "metric": metric, "count": count, "allowed": 0})
    return {"schema": 1, "totals": totals, "files": counts, "occurrences": occurrences, "allowed_occurrences": allowed_occurrences, "synchronous_captures": synchronous, "exemptions": exemptions, "findings": findings, "notes": ["raw_new/raw_delete are one ownership category; raw_handle is the supplemental ownership design section 5 metric", "unsafe_capture includes ambiguous lifetime candidates; inspect requires-review entries before approving a baseline", "ownership_allow entries are exact canonical path, named scope, metric and maximum count; excess occurrences still fail"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--baseline", default="scripts/layers/ownership_baseline.json")
    parser.add_argument("--write-baseline", action="store_true")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--final", action="store_true", help="also enforce the phase-one ownership design targets")
    parser.add_argument("--output")
    args = parser.parse_args()
    root = repo_root(args.repo)
    baseline_path = root / args.baseline
    baseline = json.loads(baseline_path.read_text(encoding="utf-8")) if baseline_path.exists() else None
    if args.strict and baseline is None:
        parser.error("strict mode requires an existing, reviewed baseline")
    report = inspect(root, baseline, args.final)
    if args.write_baseline:
        emit_json({"schema": 1, "totals": report["totals"], "files": report["files"]}, str(baseline_path))
    emit_json(report, args.output)
    return int((args.strict or args.final) and bool(report["findings"]))


if __name__ == "__main__":
    raise SystemExit(main())
