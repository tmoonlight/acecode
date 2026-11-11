"""Build the ACECode skill ZIP from the v2.9.0 checkout after pnpm install and core build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
COMMIT = "f08763d11d0202a8a8f52b5dedda6d1b2e2ebac8"
RELEASE = "https://github.com/Egonex-AI/Understand-Anything/releases/download/v2.9.0"
VIEWER_SHA = "a8626ff3ad90041e807bfdb8994eefdd986e891593c4759d08222667e5405330"


def copy_files(source, target, files):
    for path in files:
        dest = target / path.relative_to(source)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def dependencies(plugin, root):
    installed = {}
    def collect(name, parent):
        # pnpm exposes direct dependencies next to the real package.
        pkg = next((base / name for base in [parent / "node_modules", parent.parent,
                    *[p / "node_modules" for p in parent.parents]]
                    if (base / name / "package.json").is_file()), None)
        if not pkg:
            raise ValueError(f"Missing runtime dependency {name} from {parent}")
        pkg = pkg.resolve()
        manifest = load(pkg / "package.json")
        if name in installed:
            if installed[name] != manifest["version"]:
                raise ValueError(f"Incompatible versions for {name}")
            return
        installed[name] = manifest["version"]
        dest = root / "node_modules" / name
        files = [pkg / "package.json"]
        if name == "@understand-anything/core":
            files += [p for p in (pkg / "dist").rglob("*.js")
                      if "__tests__" not in p.parts and not p.name.endswith(".test.js")]
            if pkg / "dist/index.js" not in files:
                raise ValueError("Build @understand-anything/core first")
        elif "tree-sitter" in name and name != "web-tree-sitter":
            grammars = list(pkg.glob("*.wasm"))
            if not grammars:
                raise ValueError(f"No WASM grammar in {name}")
            files += grammars
        else:
            files += [p for p in pkg.rglob("*") if p.is_file()
                      and not {"node_modules", "debug", "tests", "__tests__", "test", "src"}.intersection(p.relative_to(pkg).parts[:-1])
                      and p.suffix in (".js", ".mjs", ".cjs", ".json", ".wasm")]
        files += [p for p in pkg.iterdir() if p.is_file() and
                  p.name.lower().startswith(("license", "licence", "copying", "notice", "build.md"))]
        copy_files(pkg, dest, files)
        if "tree-sitter" not in name:
            for dependency in manifest.get("dependencies", {}):
                collect(dependency, pkg)
    for name in load(plugin / "package.json")["dependencies"]:
        collect(name, plugin)
    return installed


def assemble(source, root, tgz):
    plugin = source / "understand-anything-plugin"
    if subprocess.check_output(["git","-C",source,"rev-parse","HEAD"], text=True).strip() != COMMIT:
        raise ValueError("Expected the pinned v2.9.0 source commit")
    shutil.copytree(plugin / "skills", root / "skills")
    shutil.copytree(plugin / "agents", root / "agents")
    shutil.copyfile(source / "LICENSE", root / "LICENSE")
    shutil.copyfile(plugin / "package.json", root / "package.json")
    versions = dependencies(plugin, root)
    (root / "UPSTREAM.json").write_text(json.dumps(dict(version="2.9.0", commit=COMMIT,
        source="https://github.com/Egonex-AI/Understand-Anything", viewerSHA256=VIEWER_SHA,
        dependencies=versions), indent=2) + "\n", encoding="utf-8")
    if hashlib.sha256(tgz.read_bytes()).hexdigest() != VIEWER_SHA:
        raise ValueError("Viewer archive checksum mismatch")
    with tarfile.open(tgz) as archive:
        for member in archive.getmembers():
            path = Path(member.name)
            if not member.isfile() or path.parts[0] != "package" or ".." in path.parts:
                raise ValueError(f"Unexpected viewer entry {member.name}")
            dest = root / "viewer" / Path(*path.parts[1:])
            dest.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as stream, dest.open("wb") as out:
                shutil.copyfileobj(stream, out)
    # Keep data files per run while the parser still resolves imports against the real checkout.
    batch = root / "skills/understand/compute-batches.mjs"
    replace_once(batch, "const uaDir = resolveUaDir(projectRoot);",
                 "const uaDir = process.env.ACE_UA_BATCH_DIR || resolveUaDir(projectRoot);")
    scanner = root / "skills/understand/scan-project.mjs"
    replace_once(scanner, "extname, relative, sep", "extname, relative, isAbsolute, sep")
    replace_once(scanner, "  const candidates = enumerateFiles(projectRoot);",
                 "  const physicalRoot = realpathSync(projectRoot);\n"
                 "  const candidates = enumerateFiles(projectRoot);")
    replace_once(scanner, "    const isIgnoredCombined = combined.isIgnored(rel);",
                 '    if (rel.split(/[\\\\/]/).some(p => p === ".ua" || p === ".understand-anything")) continue;\n'
                 '    const abs = resolve(projectRoot, rel);\n'
                 '    if (abs === pluginRoot || abs.startsWith(pluginRoot + sep)) continue;\n'
                 '    try {\n'
                 '      const physical = realpathSync(abs);\n'
                 '      const outside = relative(physicalRoot, physical);\n'
                 '      if (outside === ".." || outside.startsWith(".." + sep) || isAbsolute(outside)) continue;\n'
                 '    } catch { continue; }\n'
                 '    const isIgnoredCombined = combined.isIgnored(rel);')
    for relative in ("understand-domain/extract-domain-context.py",
                     "understand-knowledge/parse-knowledge-base.py",
                     "understand-knowledge/merge-knowledge-graph.py"):
        replace_once(root / "skills" / relative, "    return legacy if legacy.is_dir() else root / \".ua\"",
                     '    if os.environ.get("ACE_UA_SPECIAL_DIR"):\n'
                     '        return Path(os.environ["ACE_UA_SPECIAL_DIR"])\n'
                     '    return legacy if legacy.is_dir() else root / ".ua"')
    # The standalone domain workflow may precede any code analysis.
    viewer = root / "viewer/bin/viewer.mjs"
    replace_once(viewer, '.find((d) => fs.existsSync(path.join(d, "knowledge-graph.json")))',
                 '.find((d) => ["knowledge-graph.json", "domain-graph.json"].some(f => fs.existsSync(path.join(d, f))))')
    replace_once(viewer, 'const candidate = path.join(graphDir, fileName);',
                 'let candidate = path.join(graphDir, fileName);\n'
                 '  if (fileName === "knowledge-graph.json" && !fs.existsSync(candidate)) {\n'
                 '    candidate = path.join(graphDir, "domain-graph.json");\n  }')
    # Collect attribution files from the pinned dashboard dependency installation as well.
    for pkg in (plugin / "node_modules/.pnpm").glob("*/node_modules/*"):
        if pkg.name.startswith(".") or pkg.is_symlink() or not pkg.is_dir():
            continue
        candidates = pkg.iterdir() if pkg.name.startswith("@") else [pkg]
        for actual in candidates:
            if not (actual / "package.json").is_file():
                continue
            m = load(actual / "package.json")
            dest = root / "licenses" / (m["name"].replace("/", "__") + "-" + m["version"])
            copy_files(actual, dest, [p for p in actual.iterdir() if p.is_file()
                       and p.name.lower().startswith(("license", "licence", "copying", "notice"))])
    shutil.copytree(HERE / "bundle", root, dirs_exist_ok=True)


def replace_once(path, original, replacement):
    text = path.read_text(encoding="utf-8")
    if text.count(original) != 1:
        raise ValueError(f"Unexpected upstream source in {path.name}")
    path.write_text(text.replace(original, replacement), encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--viewer", type=Path, help="Previously downloaded official viewer tgz")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        tgz = args.viewer
        if not tgz:
            tgz = temp / "viewer.tgz"
            urllib.request.urlretrieve(RELEASE + "/understand-anything-viewer.tgz", tgz)
        root = temp / "understand-anything"
        assemble(args.source.resolve(), root, tgz)
        fd, filename = tempfile.mkstemp(dir=args.output.parent, suffix=".zip")
        os.close(fd)
        try:
            with zipfile.ZipFile(filename, "w", zipfile.ZIP_DEFLATED, compresslevel=7) as out:
                for f in sorted(root.rglob("*")):
                    if f.is_file():
                        out.write(f, f.relative_to(temp))
            os.replace(filename, args.output)
        finally:
            Path(filename).unlink(missing_ok=True)
    print(args.output.resolve())


if __name__ == "__main__":
    main()
