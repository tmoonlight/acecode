"""Run domain, wiki and Figma graph workflows with isolated intermediate files."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import shutil
import subprocess
import sys

from ua import ROOT, GRAPH, data_dir, digest, new_run, node, read, revision, write


def start(mode, project, key=None, full=False):
    data = data_dir(project)
    output = "domain-graph.json" if mode == "domain" else GRAPH
    graph = data / GRAPH
    existing = read(graph) if graph.is_file() else {}
    if existing and mode in ("knowledge", "figma"):
        kind = "design" if mode == "figma" else "knowledge"
        if existing.get("kind", "codebase") != kind:
            raise ValueError(f"This directory already has a different type of graph. Select a separate target.")
    run = new_run(data)
    scratch = run / ".ua"
    work = scratch / "intermediate"
    context = work / "scan-manifest.json"
    env = dict(os.environ, ACE_UA_SPECIAL_DIR=str(scratch))
    write(run / "special.json", dict(project=str(project), mode=mode, output=output,
                                     baseline=digest(data / output)))
    if mode == "domain":
        if existing and not full:
            context = graph
        else:
            subprocess.run([sys.executable, ROOT / "skills/understand-domain/extract-domain-context.py", project],
                           env=env, check=True)
            context = work / "domain-context.json"
    elif mode == "knowledge":
        subprocess.run([sys.executable, ROOT / "skills/understand-knowledge/parse-knowledge-base.py", project],
                       env=env, check=True)
    else:
        if not key:
            raise ValueError("Supply a Figma URL or file key")
        for name in (GRAPH, "meta.json"):
            if (data / name).exists():
                shutil.copyfile(data / name, scratch / name)
        if full:
            env["UNDERSTAND_FIGMA_FORCE"] = "1"
        node(ROOT / "skills/understand-figma/figma-scan.mjs", run, key, env=env)
        if not context.exists():
            return finish(run, cached=True)
    return {"status": "ready", "run": str(run), "work": str(work), "context": str(context)}


def finish(run, cached=False):
    state = read(run / "special.json")
    project = Path(state["project"])
    data = data_dir(project)
    mode = state["mode"]
    scratch = run / ".ua"
    work = scratch / "intermediate"
    meta = None
    for batch in work.glob("analysis-batch-*.json"):
        write(batch, read(batch))
    if mode == "domain":
        staged = work / "domain-analysis.json"
        write(staged, read(staged))
    elif mode == "knowledge":
        subprocess.run([sys.executable, ROOT / "skills/understand-knowledge/merge-knowledge-graph.py", project],
                       env=dict(os.environ, ACE_UA_SPECIAL_DIR=str(scratch)), check=True)
        staged = work / "assembled-graph.json"
        graph = read(staged)
        meta = dict(version=graph["version"], lastAnalyzedAt=datetime.now(timezone.utc).isoformat(),
                    gitCommitHash=revision(project), analyzedFiles=sum(n["type"] == "article" for n in graph["nodes"]))
    else:
        if not cached:
            node(ROOT / "skills/understand-figma/figma-merge.mjs", run)
        staged = scratch / GRAPH
        meta = read(scratch / "meta.json")
    node(ROOT / "scripts/validate.mjs", staged)
    out = data / state["output"]
    lock = data / "acecode-publish.lock"
    with lock.open("x") as owner:
        owner.write(str(os.getpid()))
    try:
        if digest(out) != state["baseline"]:
            raise ValueError("Graph changed during this run. Start a fresh analysis.")
        os.replace(staged, out)
        if meta is not None:
            write(data / "meta.json", meta)
    finally:
        lock.unlink(missing_ok=True)
    if run.parent.resolve() == (data / "tmp").resolve() and run.name.startswith("ace-"):
        shutil.rmtree(run)
    return {"status": "up-to-date" if cached else "published", "graph": str(out)}


if __name__ == "__main__":
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="action", required=True)
    p = subs.add_parser("start")
    p.add_argument("mode", choices=["domain", "knowledge", "figma"])
    p.add_argument("project", type=Path)
    p.add_argument("key", nargs="?")
    p.add_argument("--full", action="store_true")
    subs.add_parser("finish").add_argument("run", type=Path)
    args = parser.parse_args()
    try:
        result = finish(args.run.resolve(strict=True)) if args.action == "finish" else start(
            args.mode, args.project.resolve(strict=True), args.key, args.full)
        print(json.dumps(result, ensure_ascii=True))
    except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))
