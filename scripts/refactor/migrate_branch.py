#!/usr/bin/env python3
"""Rehearse legacy branches in isolation and finish a tracked-only layout migration.

Five modes: rebase, patch, --apply-map, --docs, --check. See README.md for
the pipeline, generated-help and separate seed commit contracts.
"""
from __future__ import annotations

import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import sys

from layout import GROUPS, IncludeIndex, LayoutMap, load_policy, read_tsv
from migration_docs import check_seed, documents_plan, seed_plan
from migration_git import Entry, blobs, command, conflicts, entries, git, git_result, hash_blob, isolated_clone, revision, tree_commit, write_tree
from migration_paths import build_file, managed, rewrite_paths, safe_relative, semantic_issues, source_file, transform
from repo_files import emit_json, repo_root, tracked_files, tracked_path, write_bytes_if_changed
from validate_map import inspect as validate_map

HERE = Path(__file__).resolve().parent


def map_input(path: Path, files: list[str]) -> tuple[LayoutMap, str]:
    mapping = LayoutMap(read_tsv(path))
    report = validate_map(files, mapping)
    if report["findings"]:
        raise ValueError("invalid/colliding layout map: " + json.dumps(report["findings"], ensure_ascii=False))
    return mapping, sha256(path.read_bytes()).hexdigest()


def project_tree(root: Path, inventory: dict[str, Entry], mapping: LayoutMap, selected: set[str] | None = None) -> tuple[str, list[dict]]:
    """Build projected blob/tree objects in the *isolated* repository only.

    Old and new hunk content is transformed before git diff creates a patch, so
    full-index hashes refer to real blobs and git apply -3 can use them.
    """
    files = [p for p, e in inventory.items() if managed(p, e)]
    validation = validate_map(files, mapping)
    if validation["findings"]:
        raise ValueError("projection has a destination collision: " + json.dumps(validation["findings"]))
    index = IncludeIndex(files, aliases=mapping)
    chosen = {p: e for p, e in inventory.items() if selected is None or p in selected}
    if not mapping.rows:
        return write_tree(root, chosen), []
    need_content = {e.oid for p, e in chosen.items() if managed(p, e) and (source_file(p) or build_file(p)) and e.mode != "120000"}
    contents = blobs(root, need_content)
    result, issues = {}, []
    for path, entry in chosen.items():
        if not managed(path, entry):
            result[path] = entry
            continue
        target = mapping.translate(path)
        if target is None:
            # A projection is not authorization to delete unfinished P0/P2 code.
            target = path
            issues.extend(semantic_issues([path], mapping))
        if not safe_relative(target):
            raise ValueError("unsafe mapped destination: " + target)
        if entry.mode == "120000" and (target != path or source_file(path)):
            raise ValueError("cannot mechanically migrate a symlink: " + path)
        if entry.oid in contents:
            original = contents[entry.oid]
            updated, errors = transform(original, path, index, mapping)
            issues.extend(errors)
            if original != updated:
                entry = Entry(entry.mode, hash_blob(root, updated))
        result[target] = entry
    return write_tree(root, result), issues


def final_layout(inventory: dict[str, Entry]) -> bool:
    sources = [p for p, e in inventory.items() if managed(p, e) and p.startswith("src/") and p != "src/layers.tsv"]
    return bool(sources) and all(len(p.split("/")) > 3 and p.split("/")[1] in GROUPS for p in sources)


def migrate(root: Path, mode: str, source_ref: str, onto_ref: str, destination: Path, map_path: Path, *, base_ref: str | None = None, layout: str = "final", projection: bool = False) -> dict:
    source, onto = revision(root, source_ref), revision(root, onto_ref)
    base = revision(root, base_ref) if base_ref else git(root, "merge-base", source, onto).decode().strip()
    if command(root, "merge-base", "--is-ancestor", base, source, check=False).returncode:
        raise ValueError("--base must be an ancestor of the legacy source")
    before = entries(root, source)
    target_inventory = entries(root, onto)
    source_files = [p for p, e in before.items() if managed(p, e)]
    mapping, fingerprint = map_input(map_path, source_files)
    target_is_final = final_layout(target_inventory)
    if layout == "final" and not target_is_final and not projection:
        raise ValueError("--onto is not the final grouped layout. Use --layout current for a real current-base rehearsal, or explicitly --projection for a non-buildable directory fixture; unfinished P2/P3 is not a migrated baseline.")
    if projection and layout != "final":
        raise ValueError("--projection requires --layout final")
    dest = isolated_clone(root, destination)
    artifacts = dest / ".git" / "refactor-migration"
    artifacts.mkdir()
    report = {"schema": 1, "mode": mode, "source_ref": source_ref, "source_sha": source, "onto_ref": onto_ref, "onto_sha": onto, "base_sha": base, "map_sha256": fingerprint, "layout": layout, "projection": projection, "destination": str(dest), "original_refs_untouched": True, "build_validation": "not run", "integration_ready": False, "issues": []}
    report["cherry"] = git(root, "cherry", onto, source).decode("ascii").splitlines()
    try:
        target = onto
        if projection:
            target_tree, target_issues = project_tree(dest, target_inventory, mapping)
            target = tree_commit(dest, target_tree, "LAYOUT PROJECTION ONLY: mechanical paths/includes; P2 semantics and P3 acceptance NOT completed", onto)
            report["projection_target_issues"] = target_issues + semantic_issues([p for p, e in target_inventory.items() if managed(p, e)], mapping)
            report["projection_limit"] = "Synthetic directory/include projection only. No P2 extractions, deletions, cycle fixes, CMake target equivalence or build acceptance. Never use as the actual freeze baseline."
        git(dest, "update-ref", "refs/heads/migration-target", target)
        report["target_sha"] = target
        if mode == "rebase":
            changed = [os.fsdecode(p) for p in git(root, "diff", "--name-only", "--no-renames", "-z", base, source, "--").split(b"\0") if p]
            report["changed_paths"] = changed
            if layout == "final":
                report["issues"].extend(semantic_issues(changed, mapping))
            git(dest, "switch", "--create", "migration-result", source)
            result = command(dest, "rebase", "--rebase-merges", "--no-update-refs", "--onto", target, base, "migration-result", check=False)
            report["git_rebase"] = git_result(result)
        else:
            git(dest, "switch", "--create", "migration-result", target)
            changed = {os.fsdecode(p) for p in git(root, "diff", "--name-only", "--no-renames", "-z", base, source, "--").split(b"\0") if p}
            old = entries(root, base)
            excluded = {p for p in changed if not managed(p, before.get(p, old.get(p)))}
            changed -= excluded
            report["changed_paths"] = sorted(changed)
            report["excluded_nested_or_submodule_paths"] = sorted(excluded)
            report["issues"].extend({"kind": "excluded_path", "file": p, "reason": "nested-worktree or gitlink change requires a separate explicit migration"} for p in sorted(excluded))
            effective_map = mapping if layout == "final" else LayoutMap([])
            semantic = semantic_issues(sorted(changed), effective_map)
            report["issues"].extend(semantic)
            deleted = [i for i in semantic if i["kind"] == "deleted_source"]
            if deleted:
                report.update(success=False, reason="changed legacy files were deleted upstream; a mechanical patch would lose or resurrect work", git_apply=None)
            else:
                old_tree, old_issues = project_tree(dest, old, effective_map, changed)
                new_tree, new_issues = project_tree(dest, before, effective_map, changed)
                report["issues"].extend(old_issues + new_issues)
                patch_base = tree_commit(dest, old_tree, "Transformed three-way migration patch base")
                patch_head = tree_commit(dest, new_tree, "Transformed legacy branch aggregate patch", patch_base)
                git(dest, "update-ref", "refs/heads/migration-patch-base", patch_base)
                git(dest, "update-ref", "refs/heads/migration-patch-head", patch_head)
                patch = git(dest, "diff", "--binary", "--full-index", "--no-ext-diff", "--no-textconv", "--no-renames", patch_base, patch_head, "--")
                patch_file = artifacts / "migration.patch"
                patch_file.write_bytes(patch)
                bundle = artifacts / "patch-objects.bundle"
                git(dest, "bundle", "create", str(bundle), "migration-patch-base", "migration-patch-head")
                report.update(patch=str(patch_file), patch_sha256=sha256(patch).hexdigest(), patch_bytes=len(patch), patch_objects_bundle=str(bundle), patch_base_sha=patch_base, patch_head_sha=patch_head)
                if patch:
                    result = command(dest, "apply", "--3way", "--index", "--whitespace=nowarn", str(patch_file), check=False)
                    report["git_apply"] = git_result(result)
                else:
                    result = None
                    report["git_apply"] = {"returncode": 0, "stdout": "", "stderr": "", "note": "empty aggregate patch; git apply not needed"}
        report["conflicts"] = conflicts(dest)
        operation = report.get("git_apply") if mode == "patch" else report["git_rebase"]
        report["success"] = bool(operation is not None and operation["returncode"] == 0 and not report["issues"])
        report["head_sha"] = revision(dest, "HEAD")
        report["status"] = git(dest, "status", "--porcelain=v1", "--untracked-files=no").decode("utf-8", "replace")
        report["next"] = "Resolve reported conflicts/semantic extractions; run --apply-map, --docs (seed separately), --check and real platform builds before considering integration. The source ref is preserved."
    except (ValueError, RuntimeError, OSError) as error:
        report.update(success=False, error=str(error), conflicts=conflicts(dest))
    emit_json(report, str(artifacts / "report.json"))
    report["report_file"] = str(artifacts / "report.json")
    return report


def verify_destination(root: Path, relative: str, moving_sources: set[str]) -> Path:
    if not safe_relative(relative):
        raise ValueError("unsafe destination: " + relative)
    path = root / relative
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("destination leaves checkout: " + relative)
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError("symlink destination component: " + str(parent))
    if path.exists() and relative not in moving_sources:
        raise ValueError("destination already exists (including untracked files): " + relative)
    return path


def apply_map(root: Path, map_path: Path, *, dry_run: bool = False, projection: bool = False) -> dict:
    inventory = entries(root)
    files = [p for p, e in inventory.items() if managed(p, e)]
    mapping, fingerprint = map_input(map_path, files)
    index = IncludeIndex(files, aliases=mapping)
    issues = semantic_issues(files, mapping)
    updates, moves, originals = {}, {}, {}
    for path in files:
        target = mapping.translate(path) or path
        if path != target:
            moves[path] = target
        if path != target or source_file(path) or build_file(path):
            file = tracked_path(root, path, files)
            if inventory[path].mode == "120000":
                raise ValueError("cannot migrate a tracked symlink: " + path)
            original = file.read_bytes()
            originals[path] = original
            updated, errors = transform(original, path, index, mapping)
            issues.extend(errors)
            if original != updated:
                updates[path] = updated
    for old, new in moves.items():
        verify_destination(root, new, set(moves))
    report = {"schema": 1, "mode": "apply-map", "map_sha256": fingerprint, "dry_run": dry_run, "projection": projection, "moves": moves, "content_updates": sorted(updates), "issues": issues, "success": not issues, "applied": False}
    if dry_run or issues and not projection:
        return report
    # Validate the entire plan before the first mutation; no untracked delete/add.
    for path, original in originals.items():
        if (root / path).read_bytes() != original:
            raise ValueError("file changed while planning: " + path)
    for old, new in moves.items():
        (root / new).parent.mkdir(parents=True, exist_ok=True)
        git(root, "mv", "--", old, new)
    for path, updated in updates.items():
        write_bytes_if_changed(root / moves.get(path, path), originals[path], updated)
    report["applied"] = True
    return report


def docs(root: Path, map_path: Path, *, dry_run: bool = False, version: str | None = None) -> dict:
    files = tracked_files(root)
    # Parsing entries also rejects unresolved merges before planning any writes.
    entries(root)
    mapping, fingerprint = map_input(map_path, files)
    # Capture before the potentially lengthy help build. Never adopt a concurrent
    # edit as the expected original after preparing stale replacement content.
    from migration_paths import authored_doc, generated_help
    candidates = [p for p in files if p.startswith("assets/seed/") and p.endswith(("SKILL.md", "MANIFEST.json", "seed.version")) or p.endswith("default_skill_seeder_test.cpp") or not version and (authored_doc(p) or generated_help(p) or p.startswith("openspec/changes/") and p.endswith("/design.md"))]
    originals = {p: tracked_path(root, p, files).read_bytes() for p in candidates}
    updates, detail = seed_plan(root, files, mapping, version) if version else documents_plan(root, files, mapping, fingerprint)
    if not dry_run:
        for path in updates:
            if (root / path).read_bytes() != originals[path]:
                raise ValueError("file changed while planning documentation: " + path)
        for path, updated in updates.items():
            write_bytes_if_changed(root / path, originals[path], updated)
    return {"schema": 1, "mode": "docs", "map_sha256": fingerprint, "dry_run": dry_run, "files": sorted(updates), "success": True, **detail}


def check(root: Path, map_path: Path) -> dict:
    sys.path.insert(0, str(HERE.parent / "layers"))
    from check_layers import inspect as layers
    from check_ownership import inspect as ownership
    from check_doc_paths import inspect as doc_paths
    from normalize_includes import run as normalized
    files = tracked_files(root)
    unmerged = conflicts(root)
    mapping, fingerprint = map_input(map_path, files)
    policy = load_policy(root, mapping=str(map_path))
    results = {"map": validate_map(files, mapping, policy), "layers": layers(root, files, policy, transition=False, strict=True), "include_normalization": normalized(root, True), "documentation": doc_paths(root, files), "seed": check_seed(root, files)}
    baseline = root / "scripts/layers/ownership_baseline.json"
    if baseline.is_file():
        results["ownership"] = ownership(root, json.loads(baseline.read_text(encoding="utf-8")))
    else:
        results["ownership"] = {"findings": [{"reason": "reviewed ownership baseline missing"}]}
    pending, markers = [], []
    for path in files:
        if source_file(path) or build_file(path):
            data = tracked_path(root, path, files).read_bytes()
            if build_file(path) and rewrite_paths(data, mapping) != data:
                pending.append({"file": path, "reason": "build/frontend source-map paths still need migration"})
            if re.search(rb"^(?:<{7}|={7}|>{7})(?: |\r?$)", data, re.MULTILINE):
                markers.append({"file": path, "reason": "merge conflict marker"})
    results["migration_paths"] = {"findings": pending + markers + [{"file": p, "reason": "unmerged index"} for p in unmerged]}
    counts = {name: len(value.get("findings", [])) for name, value in results.items()}
    counts["include_normalization"] = results["include_normalization"]["changed_lines"] + len(results["include_normalization"]["errors"])
    return {"schema": 1, "mode": "check", "map_sha256": fingerprint, "counts": counts, "total": sum(counts.values()), "success": not any(counts.values()), "checks": results, "build_validation": "not run; a zero static result does not replace four-platform target/build/test acceptance"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", choices=("rebase", "patch"))
    parser.add_argument("ref", nargs="?")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--apply-map", action="store_true")
    modes.add_argument("--docs", action="store_true")
    modes.add_argument("--check", action="store_true")
    parser.add_argument("--repo", default=".")
    parser.add_argument("--map", default=str(HERE / "src_layout_map.tsv"))
    parser.add_argument("--onto", default="master")
    parser.add_argument("--base", help="explicit legacy ancestor, otherwise merge-base(ref, onto)")
    parser.add_argument("--destination", type=Path, help="new isolated repository outside every existing user worktree")
    parser.add_argument("--layout", choices=("current", "final"), default="final")
    parser.add_argument("--projection", action="store_true", help="explicit synthetic layout fixture; never a completed P2/P3 baseline")
    parser.add_argument("--dry-run", action="store_true", help="plan --apply-map or --docs without writing")
    parser.add_argument("--seed-version", help="with --docs, migrate ONLY seed skills/version/hash/test in a separate commit")
    parser.add_argument("--output")
    args = parser.parse_args()
    if sum((bool(args.mode), args.apply_map, args.docs, args.check)) != 1:
        parser.error("select exactly one of rebase, patch, --apply-map, --docs, --check")
    if args.mode and (not args.ref or args.destination is None):
        parser.error("rebase/patch require a ref and --destination")
    if not args.mode and (args.ref or args.destination or args.base):
        parser.error("ref, --destination and --base belong to rebase/patch")
    if args.seed_version and not args.docs or args.dry_run and not (args.docs or args.apply_map) or args.projection and (args.docs or args.check):
        parser.error("--seed-version requires --docs; --dry-run requires --docs/--apply-map; --projection requires migration/--apply-map")
    try:
        root = repo_root(args.repo)
        map_path = Path(args.map)
        if not map_path.is_absolute():
            map_path = root / map_path
        if args.mode:
            report = migrate(root, args.mode, args.ref, args.onto, args.destination, map_path, base_ref=args.base, layout=args.layout, projection=args.projection)
        elif args.apply_map:
            report = apply_map(root, map_path, dry_run=args.dry_run, projection=args.projection)
        elif args.docs:
            report = docs(root, map_path, dry_run=args.dry_run, version=args.seed_version)
        else:
            report = check(root, map_path)
    except (ValueError, RuntimeError, OSError) as error:
        report = {"schema": 1, "success": False, "error": str(error)}
    emit_json(report, args.output)
    return int(not report["success"])


if __name__ == "__main__":
    raise SystemExit(main())
