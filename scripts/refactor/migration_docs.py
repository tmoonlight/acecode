"""Plan authored documentation and a separate, versioned seed transaction."""
from __future__ import annotations

from datetime import date
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from migration_paths import authored_doc, canonical_hash, generated_help, rewrite_paths, safe_relative
from repo_files import tracked_path


def read(root: Path, path: str, files: list[str]) -> bytes:
    return tracked_path(root, path, files).read_bytes()


def replace_field(data: bytes, key: str, old: str, new: str) -> bytes:
    pattern = re.compile(rb'("' + key.encode() + rb'"\s*:\s*)' + re.escape(json.dumps(old).encode()))
    result, count = pattern.subn(lambda m: m[1] + json.dumps(new).encode(), data)
    if count != 1:
        raise ValueError(f"expected one {key}={old} field, found {count}")
    return result


def seed_version(value: str) -> tuple[date, int]:
    match = re.fullmatch(r"(\d{4}-\d{2}-\d{2})\.(\d+)", value)
    if not match:
        raise ValueError("seed version must be YYYY-MM-DD.N")
    return date.fromisoformat(match[1]), int(match[2])


def seed_plan(root: Path, files: list[str], mapping, new_version: str) -> tuple[dict[str, bytes], dict]:
    version_path, manifest_path = "assets/seed/seed.version", "assets/seed/MANIFEST.json"
    original_version = read(root, version_path, files)
    old_version = original_version.decode("utf-8").strip()
    original_manifest = read(root, manifest_path, files)
    manifest = json.loads(original_manifest)
    if manifest["bundle_version"] != old_version:
        raise ValueError("seed.version and MANIFEST bundle_version disagree")
    updates, manifest_bytes, changed_skills = {}, original_manifest, []
    registered = set()
    for item in manifest["skills"]:
        relative = item["relative_path"]
        if not safe_relative(relative):
            raise ValueError("unsafe seed relative_path: " + relative)
        path = f"assets/seed/skills/{relative}/SKILL.md"
        if path in registered:
            raise ValueError("duplicate seed manifest path: " + path)
        registered.add(path)
        original = read(root, path, files)
        if canonical_hash(original) != item["skill_md_sha256"]:
            raise ValueError("existing seed manifest hash disagrees: " + path)
        updated = rewrite_paths(original, mapping)
        if updated == original:
            continue
        block = re.compile(rb'\{[^{}]*"relative_path"\s*:\s*' + re.escape(json.dumps(relative).encode()) + rb'[^{}]*\}')
        digest = canonical_hash(updated)
        manifest_bytes, count = block.subn(lambda m: replace_field(m.group(), "skill_md_sha256", item["skill_md_sha256"], digest), manifest_bytes)
        if count != 1:
            raise ValueError("cannot locate unique skill manifest object: " + relative)
        updates[path] = updated
        changed_skills.append({"file": path, "old_sha256": item["skill_md_sha256"], "new_sha256": digest})
    for path in files:
        if path.startswith("assets/seed/skills/") and path.endswith("/SKILL.md") and path not in registered and rewrite_paths(read(root, path, files), mapping) != read(root, path, files):
            raise ValueError("mapped seed skill is not registered in MANIFEST: " + path)
    if not updates:
        return {}, {"scope": "seed-only", "version": old_version, "changed_skills": [], "hardcoded_test_literals": 0, "note": "no mapped seed references remain; no gratuitous version bump"}
    if seed_version(new_version) <= seed_version(old_version):
        raise ValueError("changed seed skills require a newer explicit --seed-version")
    updates[version_path] = original_version.replace(old_version.encode(), new_version.encode(), 1)
    updates[manifest_path] = replace_field(manifest_bytes, "bundle_version", old_version, new_version)
    test = "tests/skills/default_skill_seeder_test.cpp"
    if test not in files:
        test = mapping.translate(test)
    if test not in files:
        raise ValueError("tracked seed regression test is required for a seed bump")
    original_test = read(root, test, files)
    literal = ('"' + old_version + '"').encode()
    count = original_test.count(literal)
    if count:
        updates[test] = original_test.replace(literal, ('"' + new_version + '"').encode())
    return updates, {"scope": "seed-only", "old_version": old_version, "new_version": new_version, "changed_skills": changed_skills, "hardcoded_test_literals": count, "note": "Only the exact prior bundle version literal is replaced; older upgrade fixtures stay unchanged. The current repository uses a dynamic packaged-version assertion."}


def build_help(root: Path, files: list[str], updates: dict[str, bytes]) -> tuple[dict[str, bytes], dict]:
    """Generate in a throwaway snapshot, then copy back only tracked outputs.

    No generated asset is regex edited, and a failed builder leaves the caller's
    entire document transaction unchanged. Every input comes from the Git index.
    """
    builder = "docs/help-source/build_help.py"
    if builder not in files:
        raise ValueError("changed help source requires tracked build_help.py")
    generated = [p for p in files if generated_help(p)]
    if not generated:
        raise ValueError("help builder has no tracked generated outputs")
    tree_path = "docs/help-source/tree.json"
    if tree_path in files:
        tree = json.loads(updates.get(tree_path, read(root, tree_path, files)))
        for group in tree:
            for page in group["pages"]:
                output = "docs/help/" + page["slug"] + ".html"
                if not safe_relative(output) or output not in generated:
                    raise ValueError("help page output must already be tracked: " + output)
    with tempfile.TemporaryDirectory(prefix="acecode-help-migration-") as directory:
        stage = Path(directory)
        for path in files:
            destination = stage / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            if path.startswith(("docs/help-source/", "docs/help/")):
                destination.write_bytes(updates.get(path, read(root, path, files)))
            else:
                # The help builder only tests existence of cited source files.
                # Placeholders preserve exactly that tracked path inventory.
                destination.touch()
        result = subprocess.run([sys.executable, "-B", str(stage / builder)], cwd=stage, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180, check=False)
        if result.returncode:
            raise ValueError("help generation failed before any document was written: " + result.stderr.decode("utf-8", "replace"))
        result_files = {}
        for path in generated:
            file = stage / path
            if file.is_symlink() or not file.is_file() or not file.resolve().is_relative_to(stage.resolve()):
                raise ValueError("unsafe or missing generated output: " + path)
            content = file.read_bytes()
            if content != read(root, path, files):
                result_files[path] = content
        return result_files, {"returncode": 0, "stdout": result.stdout.decode("utf-8", "replace"), "outputs": sorted(result_files), "execution": "temporary tracked snapshot"}


def documents_plan(root: Path, files: list[str], mapping, map_sha: str) -> tuple[dict[str, bytes], dict]:
    updates = {}
    note = f'<!-- refactor-layout-map sha256:{map_sha} -->\n源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。\n\n'.encode("utf-8")
    for path in files:
        if authored_doc(path):
            original = read(root, path, files)
            updated = rewrite_paths(original, mapping, path)
        elif re.fullmatch(r"openspec/changes/(?!archive/)[^/]+/design\.md", path):
            original = read(root, path, files)
            previous = re.compile(rb'\A<!-- refactor-layout-map sha256:[a-f0-9]+ -->\r?\n[^\r\n]*\r?\n\r?\n')
            updated = note + previous.sub(b"", original, count=1)
        else:
            continue
        if updated != original:
            updates[path] = updated
    generated = None
    if any(re.fullmatch(r"docs/help-source/group\d+\.py", p) for p in updates):
        extra, generated = build_help(root, files, updates)
        updates.update(extra)
    return updates, {"scope": "authored-documents", "help_generation": generated, "seed": "excluded; use a separate --docs --seed-version VERSION invocation/commit", "openspec": "only a map note on active design.md files; archived changes and specs are unchanged"}


def check_seed(root: Path, files: list[str]) -> dict:
    if "assets/seed/MANIFEST.json" not in files:
        return {"findings": [{"file": "assets/seed/MANIFEST.json", "reason": "tracked seed manifest missing"}]}
    findings = []
    try:
        manifest = json.loads(read(root, "assets/seed/MANIFEST.json", files))
        version = read(root, "assets/seed/seed.version", files).decode().strip()
        if manifest["bundle_version"] != version:
            findings.append({"file": "assets/seed/MANIFEST.json", "reason": "bundle version mismatch"})
        for item in manifest["skills"]:
            relative = item["relative_path"]
            if not safe_relative(relative):
                raise ValueError("unsafe manifest relative_path")
            path = f"assets/seed/skills/{relative}/SKILL.md"
            if canonical_hash(read(root, path, files)) != item["skill_md_sha256"]:
                findings.append({"file": path, "reason": "skill hash mismatch"})
    except (ValueError, KeyError, OSError) as error:
        findings.append({"file": "assets/seed/MANIFEST.json", "reason": str(error)})
    return {"findings": findings}
