"""Verify the installed archive without a provider account, package manager or upstream checkout."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

NAMES = {"understand" + suffix for suffix in (
    "", "-chat", "-dashboard", "-diff", "-domain", "-explain", "-figma", "-knowledge", "-onboard")}


def write(path, data):
    path.write_text(json.dumps(data), encoding="utf-8")


def run(args, env=None, success=True):
    result = subprocess.run(args, capture_output=True, encoding="utf-8", errors="replace", env=env, timeout=90)
    if (result.returncode == 0) != success:
        raise RuntimeError(result.stdout[-500:] + "\n" + result.stderr[-1800:])
    return result.stdout


def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def registry(exe, profile, project, package):
    runtime = profile / ".acecode/run"
    address = port()
    env = dict(os.environ, USERPROFILE=str(profile), HOME=str(profile))
    p = subprocess.Popen([exe, "daemon", "--foreground", f"--port={address}", f"--run-dir={runtime}"],
                         cwd=project, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def get(route):
        req = urllib.request.Request(f"http://127.0.0.1:{address}/api/{route}",
              headers={"Authorization": "Bearer " + (runtime / "token").read_text().strip()})
        with urllib.request.urlopen(req, timeout=3) as response:
            return response.read().decode("utf-8").replace("\r\n", "\n")
    try:
        for _ in range(120):
            try:
                payload = json.loads(get("commands?workspace="))
                break
            except OSError:
                if p.poll() is not None:
                    raise RuntimeError("Isolated daemon exited")
                time.sleep(.25)
        else:
            raise RuntimeError("Isolated daemon did not start")
        names = {entry["name"] for entry in payload["skills"] if entry["name"].startswith("understand")}
        assert names == NAMES
        for name in names:
            original = (package / "skills" / name / "SKILL.md").read_text(encoding="utf-8").strip()
            assert get("skills/" + name + "/body").strip() in (original, original.split("---", 2)[2].strip())
    finally:
        p.terminate()
        p.wait(timeout=10)


def viewer(package, project, filename):
    p = subprocess.Popen(["node", package / "viewer/bin/viewer.mjs", project, "--port", str(port()), "--no-open"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8", errors="replace")
    try:
        for line in p.stdout:
            m = re.search(r"(http://127\.0\.0\.1:\d+)/\?token=([A-Za-z0-9_-]+)", line)
            if m:
                break
        else:
            raise RuntimeError("Viewer did not provide an access URL")
        with urllib.request.urlopen(m[1], timeout=4) as index:
            assert index.status == 200 and b"<html" in index.read().lower()
        with urllib.request.urlopen(m[1]+"/"+filename+"?token="+m[2], timeout=4) as response:
            assert json.load(response)["nodes"]
    finally:
        p.terminate()
        p.wait(timeout=10)


def verify(archive, exe, dest):
    profile = dest / "临时 profile"
    skills = profile / ".acecode/skills"
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        z.extractall(skills)
    package = skills / "understand-anything"
    project = dest / "示例 project"
    project.mkdir()
    if exe:
        registry(exe, profile, project, package)
    def tool(script, *args, env=None):
        out = run([sys.executable, package / "scripts" / script, *args], env)
        return json.loads(out.strip().splitlines()[-1])
    def ua(*args):
        return tool("ua.py", *args)
    def special(*args, env=None):
        return tool("special.py", *args, env=env)
    def from_run(plan, selected):
        work = Path(plan["work"])
        for idx in plan["batchIds"]:
            evidence = json.loads((work / f"structure-{idx}.json").read_text(encoding="utf-8"))
            batch = []
            edges = []
            for f in evidence["results"]:
                path = f["path"]
                assert path in selected
                node_id = "file:" + path
                def node(id_, kind, name):
                    return {"id": id_, "type": kind, "name": name, "filePath": path,
                            "tags": [], "summary": name, "complexity": "simple"}
                batch.append(node(node_id, "file", path))
                for fn in f.get("functions", []):
                    target = "function:" + path + ":" + fn["name"]
                    batch.append(node(target, "function", fn["name"]))
                    edges.append(dict(source=node_id, target=target, type="contains",
                                      weight=1, direction="forward"))
            write(work / f"batch-{idx}.json", {"nodes":batch, "edges":edges})
            batch_path = work / f"batch-{idx}.json"
            batch_path.write_bytes(b"\xef\xbb\xbf" + batch_path.read_bytes())
        ua("assemble", plan["run"])
        write(work / "presentation.json", {"project":{"name":"Fixture","languages":["javascript"],
              "frameworks":[],"description":"Graph fixture"}, "layers":[], "tour":[]})
        return ua("publish", plan["run"])
    (project / ".understand-anything").mkdir()
    (project / "main.js").write_text("import {twice} from './math.js';\nexport function total(x){return twice(x)+1}\n")
    (project / "math.js").write_text("export function twice(x){return x*2}\n")
    first = ua("prepare", project, "--language", "zh")
    assert first["files"] == 2
    imports = json.loads((Path(first["work"])/"imports.json").read_text(encoding="utf-8"))["importMap"]
    assert imports["main.js"] == ["math.js"]
    run([sys.executable, package / "scripts/ua.py", "assemble", first["run"]], success=False)
    assert not (project / ".understand-anything/knowledge-graph.json").exists()
    result = from_run(first, {"main.js","math.js"})
    assert result["nodes"] == 4
    assert ua("query", project, "twice")["nodes"][0]["filePath"] == "math.js"
    assert ua("prepare", project)["status"] == "unchanged"
    (project / "math.js").unlink()
    (project / "main.js").write_text("export function total(x){return x+1}\n")
    inc = ua("prepare", project)
    assert inc["incremental"] and inc["filesToAnalyze"] == 1
    from_run(inc, {"main.js"})
    assert not ua("query", project, "twice")["nodes"]
    viewer(package, project, "knowledge-graph.json")
    other = {"hooks":{"Stop":[{"hooks":[{"type":"command","command":"echo test"}]}]}}
    (project/".acecode").mkdir()
    write(project/".acecode/hooks.json", other)
    for mode in ("on","on"):
        ua("refresh", project, mode)
    hook_config = json.loads((project/".acecode/hooks.json").read_text(encoding="utf-8"))
    assert len(hook_config["hooks"]["SessionStart"]) == len(hook_config["hooks"]["PostToolUse"]) == 1
    run(["git", "-C", project, "init", "-q"])
    run(["git", "-C", project, "-c", "user.name=Fixture", "-c", "user.email=test@example.invalid",
         "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm", "Fixture revision"])
    for name, command, expected in (("SessionStart", "", True), ("PostToolUse", "git commit -m test", True),
                                     ("PostToolUse", "git status", False)):
        payload = {"cwd": str(project), "hook_event_name": name, "tool_input":{"command":command}}
        result = subprocess.run([sys.executable, project/".acecode/understand-anything-refresh.py"],
                                input=json.dumps(payload), capture_output=True, text=True, check=True)
        assert bool(result.stdout.strip()) == expected
    ua("refresh", project, "off")
    assert json.loads((project/".acecode/hooks.json").read_text()) == other
    wiki = dest / "wiki"
    wiki.mkdir()
    for filename,content in {"index.md":"## Concepts\n- [[Alpha]]\n- [[Beta]]",
         "Alpha.md":"# Alpha\nLinks to [[Beta]].","Beta.md":"# Beta\nLinks to [[Alpha]]."}.items():
        (wiki/filename).write_text(content,encoding="utf-8")
    wiki_run = special("start","knowledge",wiki)
    wiki_result = special("finish",wiki_run["run"])
    assert json.loads(Path(wiki_result["graph"]).read_text(encoding="utf-8"))["kind"] == "knowledge"
    domain = dest / "domain"
    domain.mkdir()
    (domain/"main.py").write_text("def ship_order(order): return order\n")
    dom_run = special("start","domain",domain)
    assert Path(dom_run["context"]).is_file()
    domain_graph = {"version":"1.0.0","project":{"name":"Order","description":"Order management",
                    "languages":["python"],"frameworks":[],"gitCommitHash":"","analyzedAt":"2026-01-01"},
                    "edges":[],"layers":[],"tour":[],"nodes":[{"id":"domain:orders","type":"domain","name":"Orders",
                    "summary":"Order management","complexity":"simple","tags":[]}]}
    write(Path(dom_run["work"])/"domain-analysis.json",domain_graph)
    special("finish",dom_run["run"])
    viewer(package, domain, "domain-graph.json")
    figma = dest / "figma"
    figma.mkdir()
    mock = dest / "fixture.mjs"
    mock.write_text('globalThis.fetch = async url => ({ok:true,json:async()=>String(url).includes("/styles")'
        '?{meta:{styles:[]}}:String(url).includes("/images")?{images:{}}:{name:"Fixture",version:"1",'
        'document:{id:"0:0",type:"DOCUMENT",name:"Fixture",children:[{id:"0:1",type:"CANVAS",'
        'name:"Page",children:[{id:"1:1",type:"FRAME",name:"Home",children:[]}]}]}}});')
    env = dict(os.environ, FIGMA_TOKEN="fixture-value", NODE_OPTIONS="--import="+mock.as_uri())
    fig_run = special("start","figma",figma,"ABC123",env=env)
    fig_result = special("finish",fig_run["run"],env=env)
    assert json.loads(Path(fig_result["graph"]).read_text(encoding="utf-8"))["kind"] == "design"
    print("Verified code, incremental refresh, graph query, viewer, hooks, domain, wiki and mocked Figma.")
    if exe:
        print("Verified nine ACECode command registrations and skill bodies.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--acecode", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        verify(args.archive, args.acecode.resolve() if args.acecode else None, Path(temp))
