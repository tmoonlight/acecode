#!/usr/bin/env python3
"""生成安卓 App 内置的运行环境(每个 ABI 一份)。

产物(Gradle 的 abi flavor 直接引用):
  android/runtime/out/<abi>/assets/runtime/rootfs.tgz        Ubuntu 小系统(Docker buildx 导出)
  android/runtime/out/<abi>/assets/runtime/acecode.tgz       Linux 版 acecode + share/ 资源
  android/runtime/out/<abi>/assets/runtime/manifest.json    两份归档的内容版本
  android/runtime/out/<abi>/jniLibs/<abi>/lib*.so           Termux 编的 proot 及其依赖

proot 等可执行文件改名成 lib*.so 放进 jniLibs,安装时会解压到 App 的
nativeLibraryDir —— 那是系统允许 App 执行文件的位置。

用法:
  python android/runtime/build_runtime.py --abi x86_64 --acecode-tar path/to/acecode-linux-x64.tar.gz
  python android/runtime/build_runtime.py --abi arm64-v8a --acecode-tar path/to/acecode-linux-arm64.tar.gz
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
CACHE = HERE / "cache"

ABIS = {
    # abi: (termux 架构名, docker 平台, 发布包名)
    "arm64-v8a": ("aarch64", "linux/arm64", "acecode-linux-arm64.tar.gz"),
    "x86_64": ("x86_64", "linux/amd64", "acecode-linux-x64.tar.gz"),
}

# deb 包内路径 -> jniLibs 里的文件名
TERMUX_FILES = {
    "proot": {"data/data/com.termux/files/usr/bin/proot": "libproot.so",
              "data/data/com.termux/files/usr/libexec/proot/loader": "libproot-loader.so"},
    "libtalloc": {"data/data/com.termux/files/usr/lib/libtalloc.so.2.5.0": "libtalloc.so"},
    "libandroid-shmem": {"data/data/com.termux/files/usr/lib/libandroid-shmem.so": "libandroid-shmem.so"},
}

# 安卓只会把 lib*.so 解压出来,所以 libtalloc.so.2 要改名;
# proot 的 DT_NEEDED 和 libtalloc 自己的 SONAME 一起改成新名字(等长或更短,原地补 \0)。
ELF_RENAMES = {"libproot.so": (b"libtalloc.so.2", b"libtalloc.so"),
               "libtalloc.so": (b"libtalloc.so.2", b"libtalloc.so")}


def log(msg: str) -> None:
    print(f"[runtime] {msg}", flush=True)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url: str, dest: Path, sha256: str | None = None) -> Path:
    if dest.exists() and (sha256 is None or sha256_file(dest) == sha256):
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    log(f"download {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "acecode-android-runtime-builder"})
    with urllib.request.urlopen(req, timeout=120) as r, tmp.open("wb") as f:
        shutil.copyfileobj(r, f)
    if sha256 is not None:
        actual = sha256_file(tmp)
        if actual != sha256:
            tmp.unlink()
            raise SystemExit(f"sha256 mismatch for {url}: {actual} != {sha256}")
    tmp.replace(dest)
    return dest


def deb_data_members(deb: Path) -> dict[str, bytes]:
    """读 .deb(ar 归档)里 data.tar.* 的普通文件,返回 {路径: 内容}。"""
    data = deb.read_bytes()
    if data[:8] != b"!<arch>\n":
        raise SystemExit(f"not a deb: {deb}")
    pos = 8
    while pos < len(data):
        header = data[pos:pos + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58])
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".xz"):
            body = lzma.decompress(body)
        elif name.endswith(".gz"):
            body = gzip.decompress(body)
        out = {}
        with tarfile.open(fileobj=io.BytesIO(body)) as tf:
            for m in tf.getmembers():
                if m.isfile():
                    out[m.name.lstrip("./")] = tf.extractfile(m).read()
        return out
    raise SystemExit(f"no data.tar in {deb}")


def rename_needed(blob: bytes, old: bytes, new: bytes, label: str) -> bytes:
    if len(new) > len(old):
        raise SystemExit("ELF rename must not grow the string")
    needle = old + b"\0"
    count = blob.count(needle)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one '{old.decode()}', found {count}")
    return blob.replace(needle, new + b"\0" * (len(old) - len(new) + 1))


def build_jnilibs(abi: str, pins: dict, out_dir: Path) -> None:
    arch = ABIS[abi][0]
    target = out_dir / "jniLibs" / abi
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for pkg, spec in pins["termux_packages"].items():
        name = f"{pkg}_{spec['version']}_{arch}.deb"
        deb = fetch(f"{pins['termux_repo']}/{spec['pool']}/{name}", CACHE / "termux" / name,
                    spec["sha256"][arch])
        members = deb_data_members(deb)
        for src, dst in TERMUX_FILES[pkg].items():
            if src not in members:
                raise SystemExit(f"{name} has no {src}")
            blob = members[src]
            if dst in ELF_RENAMES:
                blob = rename_needed(blob, *ELF_RENAMES[dst], label=dst)
            (target / dst).write_bytes(blob)
            log(f"jniLibs/{abi}/{dst} ({len(blob)} bytes)")


def docker_rootfs(abi: str, pins: dict, work: Path) -> Path:
    """buildx 直接把镜像文件系统导出成 tar,再 gzip。按 Dockerfile+镜像名缓存。"""
    platform = ABIS[abi][1]
    dockerfile_dir = HERE / "rootfs"
    key_src = (dockerfile_dir / "Dockerfile").read_bytes() + pins["ubuntu_image"].encode() + platform.encode()
    key = hashlib.sha256(key_src).hexdigest()[:16]
    cached = CACHE / "rootfs" / f"rootfs-{abi}-{key}.tar.gz"
    if cached.exists():
        log(f"rootfs cache hit {cached.name}")
        return cached
    raw = work / "rootfs.tar"
    cmd = ["docker", "buildx", "build", "--platform", platform,
           "--build-arg", f"UBUNTU_IMAGE={pins['ubuntu_image']}",
           "--output", f"type=tar,dest={raw}", str(dockerfile_dir)]
    log(" ".join(cmd))
    subprocess.run(cmd, check=True)
    cached.parent.mkdir(parents=True, exist_ok=True)
    tmp = cached.with_suffix(".part")
    with raw.open("rb") as src, gzip.open(tmp, "wb", compresslevel=6) as dst:
        shutil.copyfileobj(src, dst, 1 << 20)
    tmp.replace(cached)
    return cached


def repack_acecode(src: Path, dest: Path) -> None:
    """发布包顶层是 acecode-linux-<arch>/;只留终端程序和资源,去掉顶层目录。

    gzip 头的时间戳固定为 0:同一份输入重复打包内容哈希不变,
    手机上就不会因为重新构建 APK 而重解 acecode。
    """
    keep_roots = {"acecode", "share", "channels"}
    with tarfile.open(src, "r:gz") as tin, dest.open("wb") as raw, \
            gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=6, mtime=0) as gz, \
            tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tout:
        for m in tin.getmembers():
            parts = m.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            rel = parts[1]
            if rel.split("/", 1)[0] not in keep_roots:
                continue
            if ".." in rel.split("/") or rel.startswith("/"):
                raise SystemExit(f"unsafe path in {src}: {m.name}")
            m.name = rel
            if rel == "acecode" and m.isfile():
                with tin.extractfile(m) as binary:
                    payload = binary.read()
                for marker in (b"--require-local-token", b"/opt/acecode/android/phone_mcp.py"):
                    if marker not in payload:
                        raise SystemExit(f"{src} lacks {marker.decode()}; build acecode from the Android-enabled source")
                tout.addfile(m, io.BytesIO(payload))
                continue
            if m.islnk():
                m.linkname = m.linkname.split("/", 1)[1] if "/" in m.linkname else m.linkname
            tout.addfile(m, tin.extractfile(m) if m.isfile() else None)
    with tarfile.open(dest, "r:gz") as check:
        if "acecode" not in check.getnames():
            raise SystemExit(f"{src} has no acecode executable")


def resolve_acecode(abi: str, args) -> Path:
    if args.acecode_tar:
        return Path(args.acecode_tar).resolve()
    asset = ABIS[abi][2]
    dest = CACHE / "acecode" / args.acecode_release / asset
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["gh", "release", "download", args.acecode_release, "-R", args.repo,
                        "-p", asset, "-D", str(dest.parent), "--clobber"], check=True)
    return dest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--abi", required=True, choices=sorted(ABIS))
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--acecode-tar", help="local acecode-linux-<arch>.tar.gz (same layout as the release asset)")
    src.add_argument("--acecode-release", help="GitHub release tag containing the Android host support")
    ap.add_argument("--repo", default="tmoonlight/acecode")
    ap.add_argument("--acecode-version", default="", help="shown in the app; defaults to the archive hash")
    args = ap.parse_args()

    pins = json.loads((HERE / "pins.json").read_text(encoding="utf-8"))
    out_dir = OUT / args.abi
    assets = out_dir / "assets" / "runtime"
    # 扩展名用 .tgz:AGP 打包时会把 .gz 资源解压并去掉扩展名
    if assets.exists():
        shutil.rmtree(assets)
    assets.mkdir(parents=True)

    build_jnilibs(args.abi, pins, out_dir)

    with tempfile.TemporaryDirectory(prefix="acecode-rootfs-") as tmp:
        rootfs = docker_rootfs(args.abi, pins, Path(tmp))
    shutil.copyfile(rootfs, assets / "rootfs.tgz")

    repack_acecode(resolve_acecode(args.abi, args), assets / "acecode.tgz")

    manifest = {
        "abi": args.abi,
        "rootfs": sha256_file(assets / "rootfs.tgz")[:16],
        "acecode": sha256_file(assets / "acecode.tgz")[:16],
        "acecodeVersion": args.acecode_version or args.acecode_release or "local",
    }
    (assets / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for p in sorted(assets.iterdir()):
        log(f"assets/runtime/{p.name} {p.stat().st_size // 1024} KiB")
    log(f"manifest {manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
