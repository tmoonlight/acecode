"""Portable project analysis and graph access for the ACECode skills."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "skills/understand"
GRAPH = "knowledge-graph.json"


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=False, separators=(",", ":"))
            stream.write("\n")
        os.replace(tmp, path)
    finally:
        Path(tmp).unlink(missing_ok=True)


def data_dir(project):
    legacy = project / ".understand-anything"
    return legacy if legacy.is_dir() else project / ".ua"


def digest(path):
    if not path.is_file():
        return None
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def revision(project):
    try:
        rev = subprocess.run(["git", "-C", project, "rev-parse", "--verify", "HEAD"],
                             capture_output=True, text=True)
        return rev.stdout.strip() if rev.returncode == 0 else ""
    except FileNotFoundError:
        return ""


def node(script, *args, env=None):
    subprocess.run(["node", script, *args], check=True, env=env)


def new_run(data):
    runs = data / "tmp"
    runs.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="ace-", dir=runs))
    (run / ".ua/intermediate").mkdir(parents=True)
    return run


def scan_files(project, target):
    node(TOOLS / "scan-project.mjs", project, target)
    scan = read(target)
    files = []
    for entry in scan["files"]:
        path = Path(entry["path"])
        actual = (project / path).resolve()
        if actual.is_relative_to(project) and not actual.is_relative_to(ROOT) and not set(path.parts).intersection(
                {".ua", ".understand-anything"}) and actual.is_file():
            files.append(entry)
    scan.update(files=files, totalFiles=len(files))
    return scan


def prepare(project, full, language):
    data = data_dir(project)
    graph = data / GRAPH
    baseline = digest(graph)
    if baseline and read(graph).get("kind", "codebase") != "codebase":
        raise ValueError("This directory holds a wiki or design graph. Select a separate code project.")
    run = new_run(data)
    work = run / ".ua/intermediate"
    prev = read(data / "acecode-state.json") if (data / "acecode-state.json").is_file() else {}
    config = read(data / "config.json") if (data / "config.json").is_file() else {}
    language = language or config.get("outputLanguage") or prev.get("language") or "en"
    incremental = bool(baseline and baseline == prev.get("graph") and not full and language == prev.get("language"))
    scan = scan_files(project, work / "enumerated.json")
    hashes = {entry["path"]: digest(project / entry["path"]) for entry in scan["files"]}
    previous = prev.get("files", {}) if incremental else {}
    changed = {p for p in hashes.keys() | previous.keys() if hashes.get(p) != previous.get(p)}
    head = revision(project)
    if incremental and not changed and head == prev.get("commit"):
        # This directory contains only this invocation's scratch files.
        (work / "enumerated.json").unlink()
        for directory in (work, work.parent, run):
            directory.rmdir()
        return {"status": "unchanged", "files": len(hashes), "graph": str(graph)}
    write(work / "input.json", {"projectRoot": str(project), "files": scan["files"]})
    node(TOOLS / "extract-import-map.mjs", work / "input.json", work / "imports.json")
    imports = read(work / "imports.json")["importMap"]
    # Revisit importers when a dependency is removed, renamed or changes its public surface.
    affected = changed | {src for mapping in (prev.get("imports", {}), imports)
                           for src, deps in mapping.items() if changed.intersection(deps)}
    scan["importMap"] = imports
    write(work / "scan-result.json", scan)
    node(TOOLS / "compute-batches.mjs", project,
         env=dict(os.environ, ACE_UA_BATCH_DIR=str(work.parent)))
    plan = read(work / "batches.json")["batches"]
    expected = {}
    for batch in plan:
        files = [f for f in batch["files"] if not incremental or f["path"] in affected]
        if not files:
            continue
        index = batch["batchIndex"]
        batch.update(projectRoot=str(project), batchFiles=files, files=files)
        write(work / f"input-{index}.json", batch)
        node(TOOLS / "extract-structure.mjs", work / f"input-{index}.json", work / f"structure-{index}.json")
        expected[str(index)] = [f["path"] for f in files]
    if incremental:
        old = read(graph)
        retained = [n for n in old["nodes"] if not n.get("filePath") or
                    (n["filePath"] in hashes and n["filePath"] not in affected)]
        retained_ids = {n["id"] for n in retained}
        # Keep outbound relations of unchanged files if their targets still exist after merging.
        write(work / "batch-0.json", {"nodes": retained,
              "edges": [e for e in old["edges"] if e["source"] in retained_ids]})
    if not expected and not incremental:
        write(work / "batch-0.json", {"nodes": [], "edges": []})
    write(run / "run.json", dict(project=str(project), baseline=baseline, files=hashes,
          imports=imports, expected=expected, commit=head, language=language))
    return {"status": "ready", "run": str(run), "work": str(work), "batchIds": list(expected),
            "files": len(hashes), "filesToAnalyze": sum(map(len, expected.values())),
            "incremental": incremental, "language": language}


def assemble(run):
    state = read(run / "run.json")
    work = run / ".ua/intermediate"
    for index, paths in state["expected"].items():
        batch_path = work / f"batch-{index}.json"
        batch = read(batch_path)
        covered = {n.get("filePath") for n in batch["nodes"] if n["type"] not in ("function", "class")}
        missing = set(paths) - covered
        if missing:
            raise ValueError(f"Batch {index} lacks file nodes for: {sorted(missing)}")
        # Windows editors can write a UTF-8 BOM; upstream reads plain UTF-8.
        write(batch_path, batch)
    subprocess.run([sys.executable, TOOLS / "merge-batch-graphs.py", run], check=True)
    merged = read(work / "assembled-graph.json")
    represented = {n.get("filePath") for n in merged["nodes"] if n["type"] not in ("function", "class")}
    missing = state["files"].keys() - represented
    if missing:
        raise ValueError(f"Merged graph lacks file nodes for: {sorted(missing)}")
    return {"draft": str(work / "assembled-graph.json"), "nodes": len(merged["nodes"]), "edges": len(merged["edges"])}


def publish(run):
    state = read(run / "run.json")
    project = Path(state["project"])
    data = data_dir(project)
    work = run / ".ua/intermediate"
    assembled = read(work / "assembled-graph.json")
    info = read(work / "presentation.json")
    graph = {k: assembled[k] for k in ("nodes", "edges")}
    graph.update(version="1.0.0", kind="codebase", project=dict(info["project"]),
                 layers=info["layers"], tour=info["tour"])
    now = datetime.now(timezone.utc).isoformat()
    graph["project"].update(analyzedAt=now, gitCommitHash=state["commit"])
    staged = work / GRAPH
    write(staged, graph)
    node(ROOT / "scripts/validate.mjs", staged)
    lock = data / "acecode-publish.lock"
    with lock.open("x") as owner:
        owner.write(str(os.getpid()))
    try:
        if digest(data / GRAPH) != state["baseline"]:
            raise ValueError("The published graph changed during analysis. Run prepare again.")
        current = scan_files(project, work / "recheck.json")
        if revision(project) != state["commit"] or set(state["files"]) != {f["path"] for f in current["files"]} or any(
                digest(project / p) != sha for p, sha in state["files"].items()):
            raise ValueError("Source files changed during analysis. Run prepare again.")
        os.replace(staged, data / GRAPH)
        write(data / "acecode-state.json", {k: state[k] for k in ("files", "imports", "language", "commit")}
              | {"graph": digest(data / GRAPH)})
        write(data / "meta.json", dict(version="1.0.0", lastAnalyzedAt=now,
              gitCommitHash=state["commit"], analyzedFiles=len(state["files"])))
    finally:
        lock.unlink(missing_ok=True)
    # Only remove this successful run's own scratch directory.
    import shutil
    if run.parent.resolve() == (data / "tmp").resolve() and run.name.startswith("ace-"):
        shutil.rmtree(run)
    return {"graph": str(data / GRAPH), "nodes": len(graph["nodes"]), "edges": len(graph["edges"])}


def query(project, terms):
    data = data_dir(project)
    g = read(data / GRAPH)
    head = revision(project)
    out = {"project": g["project"], "commitChanged": head != g["project"].get("gitCommitHash"),
           "counts": {k: len(g[k]) for k in ("nodes", "edges", "layers", "tour")}}
    if not terms:
        out["layers"] = [{k:v for k,v in layer.items() if k != "nodeIds"} for layer in g["layers"]]
        out["tour"] = g["tour"][:8]
        return out
    needle = terms.casefold()
    def score(n):
        fields = [n.get(k, "").casefold() for k in ("name", "id", "filePath", "summary")]
        if needle in fields[:3]:
            return 3
        if any(needle in field for field in fields[:3]):
            return 2
        return int(needle in fields[3])
    ranked = sorted(((score(n), n) for n in g["nodes"]), key=lambda pair: -pair[0])
    nodes = [n for s, n in ranked if s][:20]
    ids = {n["id"] for n in nodes}
    out.update(nodes=nodes, edges=[e for e in g["edges"] if ids.intersection((e["source"], e["target"]))][:80])
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    cmds = p.add_subparsers(dest="command", required=True)
    for name in ("prepare", "query", "dashboard", "paths", "refresh"):
        sub = cmds.add_parser(name)
        sub.add_argument("project", type=Path)
        if name == "prepare":
            sub.add_argument("--full", action="store_true")
            sub.add_argument("--language")
        if name == "query":
            sub.add_argument("terms", nargs="?", default="")
        if name == "dashboard":
            sub.add_argument("--no-open", action="store_true")
            sub.add_argument("--port", type=int)
        if name == "refresh":
            sub.add_argument("mode", choices=["on", "off"])
    for name in ("assemble", "publish"):
        cmds.add_parser(name).add_argument("run", type=Path)
    args = p.parse_args()
    if hasattr(args, "run"):
        run = args.run.resolve(strict=True)
        result = assemble(run) if args.command == "assemble" else publish(run)
    else:
        project = args.project.resolve(strict=True)
        if not project.is_dir():
            raise ValueError("Expected a project directory")
        if args.command == "prepare":
            result = prepare(project, args.full, args.language)
        elif args.command == "query":
            result = query(project, args.terms)
        elif args.command == "dashboard":
            flags = ["--no-open"] if args.no_open else []
            if args.port is not None:
                flags.extend(["--port", str(args.port)])
            node(ROOT / "viewer/bin/viewer.mjs", project, *flags)
            return
        elif args.command == "refresh":
            from refresh import configure
            result = configure(project, args.mode == "on")
        else:
            result = {"project": str(project), "data": str(data_dir(project)), "package": str(ROOT)}
    print(json.dumps(result, ensure_ascii=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))
