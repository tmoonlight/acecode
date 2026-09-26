"""Private Git indexes and isolated migration repositories.

The caller's refs, index, working files and submodules are never mutated.
All historical file lists come from git ls-files against a private index.
"""
from __future__ import annotations

from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
import tempfile

from migration_paths import Entry, safe_relative


def command(root: Path, *args: str, data: bytes | None = None, env: dict | None = None, check: bool = True, timeout: int = 300) -> subprocess.CompletedProcess:
    environment = os.environ.copy()
    # Do not inherit another tool's index/worktree override.
    for key in ("GIT_INDEX_FILE", "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
        environment.pop(key, None)
    environment.update(env or {})
    result = subprocess.run(["git", "-c", "core.fsmonitor=false", "-c", "core.autocrlf=false", "-c", "core.longpaths=true", "-c", "submodule.recurse=false", "-C", str(root), *args], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment, timeout=timeout, check=False)
    if check and result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace").strip() or result.stdout.decode("utf-8", "replace").strip())
    return result


def git(root: Path, *args: str, **kwargs) -> bytes:
    return command(root, *args, **kwargs).stdout


def revision(root: Path, ref: str) -> str:
    if ref.startswith("-"):
        raise ValueError("a revision cannot start with '-'")
    return git(root, "rev-parse", "--verify", ref + "^{commit}").decode().strip()


@contextmanager
def private_index():
    with tempfile.TemporaryDirectory(prefix="acecode-migration-index-") as directory:
        yield {"GIT_INDEX_FILE": str(Path(directory) / "index")}


def entries(root: Path, ref: str | None = None) -> dict[str, Entry]:
    def read(env=None):
        result = {}
        for record in git(root, "ls-files", "--stage", "-z", env=env).split(b"\0"):
            if not record:
                continue
            header, raw_path = record.split(b"\t", 1)
            mode, oid, stage = header.decode("ascii").split()
            path = os.fsdecode(raw_path)
            if stage != "0":
                raise ValueError("unmerged index entry: " + path)
            if not safe_relative(path):
                raise ValueError("unsafe indexed path: " + path)
            result[path] = Entry(mode, oid)
        return result
    if ref is None:
        return read()
    with private_index() as env:
        git(root, "read-tree", ref, env=env)
        return read(env)


def blobs(root: Path, oids: set[str]) -> dict[str, bytes]:
    if not oids:
        return {}
    ordered = sorted(oids)
    output = git(root, "cat-file", "--batch", data=("\n".join(ordered) + "\n").encode("ascii"))
    result, offset = {}, 0
    for oid in ordered:
        end = output.index(b"\n", offset)
        actual, kind, size = output[offset:end].decode("ascii").split()
        if actual != oid or kind != "blob":
            raise ValueError("expected regular blob " + oid)
        offset = end + 1
        result[oid] = output[offset:offset + int(size)]
        offset += int(size) + 1
    return result


def hash_blob(root: Path, data: bytes) -> str:
    return git(root, "hash-object", "-w", "--stdin", data=data).decode().strip()


def write_tree(root: Path, files: dict[str, Entry]) -> str:
    with private_index() as env:
        git(root, "read-tree", "--empty", env=env)
        records = b"".join(f"{entry.mode} {entry.oid}\t".encode("ascii") + os.fsencode(path) + b"\0" for path, entry in sorted(files.items()))
        git(root, "update-index", "-z", "--index-info", data=records, env=env)
        return git(root, "write-tree", env=env).decode().strip()


def tree_commit(root: Path, tree: str, message: str, parent: str | None = None) -> str:
    args = ["commit-tree", tree]
    if parent:
        args.extend(("-p", parent))
    return git(root, *args, data=(message + "\n").encode("utf-8")).decode().strip()


def isolated_clone(source: Path, destination: Path) -> Path:
    destination = destination.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError("destination must not exist: " + str(destination))
    if destination.resolve().is_relative_to(source.resolve()):
        raise ValueError("destination must be outside the source checkout")
    # Refuse nesting inside *any* user worktree, including paths hidden by .gitignore.
    for record in git(source, "worktree", "list", "--porcelain", "-z").split(b"\0"):
        if record.startswith(b"worktree "):
            worktree = Path(os.fsdecode(record[len(b"worktree "):])).resolve()
            if destination.resolve().is_relative_to(worktree):
                raise ValueError("destination is inside a user worktree: " + str(worktree))
    destination.parent.mkdir(parents=True, exist_ok=True)
    git(source, "clone", "--shared", "--no-checkout", "--", str(source), str(destination))
    git(destination, "config", "core.autocrlf", "false")
    git(destination, "config", "core.longpaths", "true")
    git(destination, "config", "core.hooksPath", str(destination / ".git" / "migration-no-hooks"))
    for key, fallback in (("user.name", "ACECode migration rehearsal"), ("user.email", "migration@example.invalid")):
        value = command(source, "config", "--get", key, check=False).stdout.decode("utf-8", "replace").strip()
        git(destination, "config", key, value or fallback)
    git(destination, "remote", "set-url", "--push", "origin", "disabled://migration-read-only")
    return destination


def git_result(result: subprocess.CompletedProcess) -> dict:
    return {"returncode": result.returncode, "stdout": result.stdout.decode("utf-8", "replace"), "stderr": result.stderr.decode("utf-8", "replace")}


def conflicts(root: Path) -> list[str]:
    return [os.fsdecode(p) for p in git(root, "diff", "--name-only", "--diff-filter=U", "-z").split(b"\0") if p]
