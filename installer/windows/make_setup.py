#!/usr/bin/env python3
"""Embed an ACECode Windows package into acecode-setup.exe.

The stub (acecode-setup.exe built by CMake) is copied to the output path and three
RT_RCDATA resources are added with the Windows UpdateResource API:

  ACECODE_PAYLOAD       the package zip (same layout as acecode-windows-<arch>.zip)
  ACECODE_UNINSTALLER   a byte copy of the stub, installed as uninstall.exe
  ACECODE_PAYLOAD_INFO  JSON: version, target, sizes and SHA-256 of the two blobs

Sign the stub before running this script and sign the produced setup afterwards;
the embedded uninstaller then keeps the stub's signature. Only the standard library
is used (ctypes), so no third-party packaging tool is involved.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import sys
import zipfile
from pathlib import Path, PurePosixPath

PAYLOAD_RESOURCE = "ACECODE_PAYLOAD"
INFO_RESOURCE = "ACECODE_PAYLOAD_INFO"
UNINSTALLER_RESOURCE = "ACECODE_UNINSTALLER"
RT_RCDATA = 10
TARGETS = ("windows-x64", "windows-arm64")
RESERVED = re.compile(r"^(con|prn|aux|nul|com[1-9]|lpt[1-9])$", re.IGNORECASE)


def is_safe_entry(name: str) -> bool:
    """Mirror of installer::is_safe_archive_entry (src/apps/installer/payload_archive.cpp)."""
    clean = name.replace("\\", "/").rstrip("/")
    if not clean or clean.startswith("/") or ":" in clean:
        return False
    for part in clean.split("/"):
        if part in ("", ".", "..") or part.endswith((".", " ")):
            return False
        if RESERVED.match(part.split(".", 1)[0]):
            return False
    return True


def zip_directory(package_dir: Path) -> bytes:
    """Deterministic deflate zip of a staged package directory (forward-slash names)."""
    buffer = io.BytesIO()
    files = sorted(p for p in package_dir.rglob("*") if p.is_file())
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            name = PurePosixPath(*path.relative_to(package_dir).parts).as_posix()
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes(), compresslevel=9)
    return buffer.getvalue()


def validate_payload(zip_bytes: bytes) -> list[str]:
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as archive:
        names = archive.namelist()
    unsafe = [n for n in names if not is_safe_entry(n)]
    if unsafe:
        raise ValueError("unsafe entries in package: " + ", ".join(unsafe[:5]))
    roots = {n.replace("\\", "/") for n in names}
    if "acecode.exe" not in roots:
        raise ValueError("package zip must contain acecode.exe at its root")
    return names


def payload_info(version: str, target: str, payload: bytes, uninstaller: bytes) -> dict:
    if not re.fullmatch(r"[0-9]+(\.[0-9]+)*", version):
        raise ValueError(f"invalid version: {version}")
    if target not in TARGETS:
        raise ValueError(f"invalid target: {target}")
    return {
        "schema": 1,
        "version": version,
        "target": target,
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "payload_size": len(payload),
        "uninstaller_sha256": hashlib.sha256(uninstaller).hexdigest(),
    }


def project_version(repo: Path) -> str:
    text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(acecode VERSION ([0-9]+\.[0-9]+\.[0-9]+)", text)
    if not match:
        raise ValueError("cannot read the ACECode version from CMakeLists.txt")
    return match.group(1)


def _kernel32():
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    kernel32.BeginUpdateResourceW.restype = wintypes.HANDLE
    kernel32.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPCWSTR,
                                         wintypes.WORD, ctypes.c_void_p, wintypes.DWORD]
    kernel32.UpdateResourceW.restype = wintypes.BOOL
    kernel32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    kernel32.EndUpdateResourceW.restype = wintypes.BOOL
    kernel32.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel32.LoadLibraryExW.restype = wintypes.HMODULE
    kernel32.FindResourceW.argtypes = [wintypes.HMODULE, wintypes.LPCWSTR, ctypes.c_void_p]
    kernel32.FindResourceW.restype = wintypes.HANDLE
    kernel32.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel32.SizeofResource.restype = wintypes.DWORD
    kernel32.FreeLibrary.argtypes = [wintypes.HMODULE]
    return ctypes, kernel32


def embed_resources(exe: Path, resources: list[tuple[str, bytes]]) -> None:
    if os.name != "nt":
        raise RuntimeError("embedding resources needs the Windows UpdateResource API")
    ctypes, kernel32 = _kernel32()
    handle = kernel32.BeginUpdateResourceW(str(exe), False)
    if not handle:
        raise OSError(ctypes.get_last_error(), f"BeginUpdateResource failed for {exe}")
    try:
        for name, data in resources:
            buffer = ctypes.create_string_buffer(data, len(data))
            if not kernel32.UpdateResourceW(handle, RT_RCDATA, name, 0, buffer, len(data)):
                raise OSError(ctypes.get_last_error(), f"UpdateResource failed for {name}")
    except BaseException:
        kernel32.EndUpdateResourceW(handle, True)
        raise
    if not kernel32.EndUpdateResourceW(handle, False):
        raise OSError(ctypes.get_last_error(), f"EndUpdateResource failed for {exe}")


def resource_sizes(exe: Path, names: list[str]) -> dict[str, int]:
    """Read back resource sizes through the loader, the same lookup the installer uses."""
    ctypes, kernel32 = _kernel32()
    load_as_datafile = 0x00000002 | 0x00000020  # LOAD_LIBRARY_AS_DATAFILE | AS_IMAGE_RESOURCE
    module = kernel32.LoadLibraryExW(str(exe), None, load_as_datafile)
    if not module:
        raise OSError(ctypes.get_last_error(), f"cannot load {exe} as a data file")
    try:
        sizes = {}
        for name in names:
            found = kernel32.FindResourceW(module, name, RT_RCDATA)
            sizes[name] = kernel32.SizeofResource(module, found) if found else 0
        return sizes
    finally:
        kernel32.FreeLibrary(module)


def build_setup(stub: Path, payload: bytes, version: str, target: str, out: Path) -> dict:
    validate_payload(payload)
    uninstaller = stub.read_bytes()
    info = payload_info(version, target, payload, uninstaller)
    out.parent.mkdir(parents=True, exist_ok=True)
    temp = out.with_name(out.name + ".tmp")
    shutil.copyfile(stub, temp)
    try:
        embed_resources(temp, [
            (PAYLOAD_RESOURCE, payload),
            (UNINSTALLER_RESOURCE, uninstaller),
            (INFO_RESOURCE, json.dumps(info, separators=(",", ":")).encode("utf-8")),
        ])
        sizes = resource_sizes(temp, [PAYLOAD_RESOURCE, UNINSTALLER_RESOURCE])
        if sizes[PAYLOAD_RESOURCE] != len(payload) or sizes[UNINSTALLER_RESOURCE] != len(uninstaller):
            raise RuntimeError(f"embedded resource sizes do not match: {sizes}")
        os.replace(temp, out)
    finally:
        if temp.exists():
            temp.unlink()
    return info


def main(argv: list[str] | None = None) -> int:
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stub", type=Path, required=True, help="acecode-setup.exe from the CMake build")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--payload-zip", type=Path, help="acecode-windows-<arch>.zip")
    source.add_argument("--package-dir", type=Path, help="staged package directory to zip")
    parser.add_argument("--version", default=None, help="defaults to the CMake project version")
    parser.add_argument("--target", default="windows-x64", choices=TARGETS)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args(argv)

    version = args.version or project_version(repo)
    payload = args.payload_zip.read_bytes() if args.payload_zip else zip_directory(args.package_dir)
    out = args.out or Path.cwd() / f"ACECode-{version}-{args.target}-setup.exe"
    info = build_setup(args.stub, payload, version, args.target, out)
    print(json.dumps({"setup": str(out), "size": out.stat().st_size, **info}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
