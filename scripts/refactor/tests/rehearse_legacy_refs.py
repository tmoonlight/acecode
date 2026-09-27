#!/usr/bin/env python3
"""Reproduce P2-09 against the nine fixed P0-02 legacy heads in new repositories."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from hashlib import sha256
import json
from pathlib import Path
import sys
import tempfile

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from migrate_branch import migrate
from migration_git import git, revision
from repo_files import emit_json, repo_root


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--base", required=True)
    parser.add_argument("--inventory", default="openspec/changes/refactor20260927-restructure-src-layers/branch-inventory.json")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--scenario", action="append", choices=("current-patch", "current-rebase", "projection-patch"), help="repeat to select scenarios; default all three")
    parser.add_argument("--jobs", type=int, choices=(1, 2, 3), default=1, help="independent isolated repositories; source remains read-only")
    args = parser.parse_args()
    root = repo_root(args.repo)
    base = revision(root, args.base)
    inventory = json.loads((root / args.inventory).read_bytes())
    refs = [r for r in inventory["branches"] if r["ref"].startswith("refs/remotes/") and r["unique_commits"] and r["src_paths"]]
    if len(refs) != 9:
        raise ValueError(f"expected the nine reviewed P0-02 refs, got {len(refs)}")
    for ref in refs:
        if revision(root, ref["ref"]) != ref["head"]:
            raise ValueError("legacy ref changed since inventory; use an explicitly reviewed new inventory: " + ref["ref"])
    pool = Path(tempfile.mkdtemp(prefix="acecode-p209-rehearsal-"))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    original_diff = sha256(git(root, "diff", "--binary", "HEAD", "--")).hexdigest()
    original_refs = {r["ref"]: revision(root, r["ref"]) for r in refs}
    original_refs["master"] = revision(root, "master")
    index_path = Path(git(root, "rev-parse", "--path-format=absolute", "--git-path", "index").decode().strip())
    original_index = sha256(index_path.read_bytes()).hexdigest()
    report = {"schema": 1, "fixed_base": base, "tool_head": revision(root, "HEAD"), "inventory_base": inventory["base_sha"], "started_utc": datetime.now(timezone.utc).isoformat(), "isolated_root": str(pool), "runs": []}
    scenarios = args.scenario or ["current-patch", "current-rebase", "projection-patch"]
    if len(scenarios) != len(set(scenarios)):
        raise ValueError("duplicate --scenario")
    work = [(number, ref, label) for number, ref in enumerate(refs, 1) for label in scenarios]
    def run(item):
        number, ref, label = item
        mode, projection = ("rebase" if label.endswith("rebase") else "patch"), label.startswith("projection")
        name = f"{number:02d}-{label}"
        result = migrate(root, mode, ref["head"], base, pool / name, HERE / "src_layout_map.tsv", layout="final" if projection else "current", projection=projection)
        return ref, label, mode, name, result
    with ThreadPoolExecutor(max_workers=args.jobs) as workers:
        for ref, label, mode, name, result in workers.map(run, work):
            result["inventory_ref"] = ref["ref"]
            emit_json(result, str(args.output_dir / (name + ".json")))
            operation = result.get("git_apply") if mode == "patch" else result.get("git_rebase")
            summary = {"ref": ref["ref"], "source_sha": ref["head"], "scenario": label, "report": name + ".json", "git_returncode": operation.get("returncode") if operation else None, "success": result["success"], "conflicts": result.get("conflicts", []), "issues": result.get("issues", []), "reason": result.get("reason", result.get("error"))}
            report["runs"].append(summary)
            emit_json(report, str(args.output_dir / "manifest.json"))
            print(json.dumps({"ref": ref["ref"], "scenario": label, "git_returncode": summary["git_returncode"], "conflicts": len(summary["conflicts"]), "issues": len(summary["issues"]), "reason": summary["reason"]}, ensure_ascii=True), flush=True)
    report["original_refs_unchanged"] = all(revision(root, ref) == oid for ref, oid in original_refs.items())
    report["source_index_unchanged"] = original_index == sha256(index_path.read_bytes()).hexdigest()
    report["source_working_diff_unchanged"] = original_diff == sha256(git(root, "diff", "--binary", "HEAD", "--")).hexdigest()
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    report["limit"] = "Current modes use the real pre-P2/P3 base. Projection is explicitly synthetic and not a build/freeze acceptance. A Git failure or semantic extraction requires manual porting; none of these feature branches is approved or merged."
    emit_json(report, str(args.output_dir / "manifest.json"))
    return int(not all(report[k] for k in ("original_refs_unchanged", "source_index_unchanged", "source_working_diff_unchanged")))


if __name__ == "__main__":
    raise SystemExit(main())
