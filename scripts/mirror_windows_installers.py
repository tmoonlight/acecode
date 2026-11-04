#!/usr/bin/env python3
"""Mirror verified Windows setup downloads without changing aceupdate.json."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
from urllib.parse import quote
from urllib.request import Request, urlopen

from verify_windows_setup import verify_setup


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def preflight(assets, version):
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("Expected a stable numeric version")
    assets = Path(assets)
    checksums = {}
    for line in (assets / "SHA256SUMS.txt").read_text(encoding="utf-8-sig").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64})\s+\*?(.+)", line)
        if match:
            name = match[2].removeprefix("./")
            if name in checksums:
                raise ValueError(f"Duplicate release checksum: {name}")
            checksums[name] = match[1].lower()
    result = []
    for arch in ("x64", "arm64"):
        name = f"ACECode-{version}-windows-{arch}-setup.exe"
        payload = assets / f"acecode-windows-{arch}.zip"
        setup = assets / name
        for path in (setup, payload):
            if not path.is_file() or path.stat().st_size == 0:
                raise ValueError(f"Missing or empty release asset: {path.name}")
            if checksums.get(path.name) != digest(path):
                raise ValueError(f"Release checksum mismatch: {path.name}")
        verify_setup(setup, payload, version, f"windows-{arch}")
        result.append({"source": str(setup), "file": name,
                       "alias": f"ACECode-windows-{arch}-setup.exe",
                       "size": setup.stat().st_size, "sha256": checksums[name]})
    return result


def atomic_copy(source, destination):
    destination = Path(destination)
    fd, temporary = tempfile.mkstemp(prefix=".setup-publish-", dir=destination.parent)
    os.close(fd)
    try:
        shutil.copyfile(source, temporary)
        if digest(source) != digest(temporary):
            raise ValueError("Staged installer hash mismatch")
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def verify_public(url, expected):
    request = Request(url, headers={"Accept": "application/octet-stream", "Cache-Control": "no-cache"})
    with urlopen(request, timeout=120) as response:
        if response.status != 200 or response.headers.get_content_type() != "application/octet-stream":
            raise ValueError(f"Unexpected installer HTTP response: {url}")
        result, size = hashlib.sha256(), 0
        while chunk := response.read(1024 * 1024):
            result.update(chunk)
            size += len(chunk)
    if size != expected["size"] or result.hexdigest() != expected["sha256"]:
        raise ValueError(f"Public installer body mismatch: {url}")


def publish(assets, version, update_dir, base_url):
    definitions = preflight(assets, version)
    update_dir = Path(update_dir).resolve()
    update_dir.mkdir(parents=True, exist_ok=True)
    manifest = update_dir / "aceupdate.json"
    before = manifest.read_bytes() if manifest.exists() else None
    for item in definitions:
        for name in (item["file"], item["alias"]):
            atomic_copy(item["source"], update_dir / name)
    for item in definitions:
        for name in (item["file"], item["alias"]):
            verify_public(base_url.rstrip("/") + "/" + quote(name), item)
        item["public_verified"] = True
    after = manifest.read_bytes() if manifest.exists() else None
    if before != after:
        raise RuntimeError("Updater manifest changed concurrently during installer mirroring")
    return definitions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--update-dir", type=Path, default=Path("J:/jenkins_green/aupdate"))
    parser.add_argument("--base-url", default="http://2017studio.imwork.net:82/aupdate/")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    result = publish(args.assets, args.version, args.update_dir, args.base_url)
    output = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.report:
        args.report.write_text(output, encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
