#!/usr/bin/env python3
"""Verify native setup resources against the exact Windows release ZIP."""

import argparse
import ctypes
from contextlib import ExitStack
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_setup", ROOT / "installer/windows/make_setup.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)
MACHINES = {"windows-x64": 0x8664, "windows-arm64": 0xAA64}


def pe_machine(data):
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Not a PE executable")
    offset = struct.unpack_from("<I", data, 60)[0]
    if offset + 6 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError("Invalid PE header")
    return struct.unpack_from("<H", data, offset + 4)[0]


def read_resources(path):
    from ctypes import wintypes
    _, kernel = packager._kernel32()
    kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.LoadResource.restype = wintypes.HANDLE
    kernel.LockResource.argtypes = [wintypes.HANDLE]
    kernel.LockResource.restype = ctypes.c_void_p
    module = kernel.LoadLibraryExW(str(Path(path).resolve()), None, 0x22)
    if not module:
        raise OSError(ctypes.get_last_error(), "Cannot read setup resources")
    try:
        result = {}
        for name in (packager.PAYLOAD_RESOURCE, packager.INFO_RESOURCE, packager.UNINSTALLER_RESOURCE):
            resource = kernel.FindResourceW(module, name, packager.RT_RCDATA)
            size = kernel.SizeofResource(module, resource) if resource else 0
            loaded = kernel.LoadResource(module, resource) if size else None
            pointer = kernel.LockResource(loaded) if loaded else None
            if not pointer:
                raise ValueError(f"Missing resource: {name}")
            result[name] = ctypes.string_at(pointer, size)
        return result
    finally:
        kernel.FreeLibrary(module)


def verify_contents(setup, resources, payload, version, target):
    expected_machine = MACHINES[target]
    if pe_machine(setup) != expected_machine:
        raise ValueError("Setup architecture mismatch")
    embedded = resources[packager.PAYLOAD_RESOURCE]
    uninstaller = resources[packager.UNINSTALLER_RESOURCE]
    info = json.loads(resources[packager.INFO_RESOURCE])
    expected = packager.payload_info(version, target, payload, uninstaller)
    if info != expected:
        raise ValueError("Setup metadata or resource hash mismatch")
    if embedded != payload:
        raise ValueError("Setup payload differs from release ZIP")
    if pe_machine(uninstaller) != expected_machine:
        raise ValueError("Uninstaller architecture mismatch")
    packager.validate_payload(embedded)
    with zipfile.ZipFile(io.BytesIO(embedded)) as archive:
        for name in ("acecode.exe", "acecode-desktop.exe", "acecode-computer-use.exe"):
            if pe_machine(archive.read(name)) != expected_machine:
                raise ValueError(f"Payload architecture mismatch: {name}")
    return info


def verify_setup(setup, payload_zip, version, target):
    return verify_contents(Path(setup).read_bytes(), read_resources(setup),
                           Path(payload_zip).read_bytes(), version, target)


def smoke_install(setup, version, install_volume=None):
    """Use the installer's test sandbox; never touch the user's installation."""
    import winreg
    registry = r"Software\ACECode-setup-test\Uninstall\ACECode"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry):
            raise RuntimeError("Another installer sandbox is registered; finish it first")
    except FileNotFoundError:
        pass
    with ExitStack() as stack:
        root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="acecode-release-install-"))).resolve()
        install_root = root / "programs"
        if install_volume:
            volume = Path(install_volume).resolve()
            install_root = Path(stack.enter_context(tempfile.TemporaryDirectory(
                prefix="acecode-release-install-", dir=volume))).resolve()
            if install_root.parent != volume:
                raise ValueError("Installation sandbox escaped its chosen volume")
        profile = root / "profile"
        sentinel = profile / ".acecode/user-data-sentinel.txt"
        sentinel.parent.mkdir(parents=True)
        sentinel.write_text("preserve user data", encoding="utf-8")
        installed = install_root / "ACECode"
        temporary = root / "tmp"
        temporary.mkdir()
        env = dict(os.environ, USERPROFILE=str(profile), ACECODE_SETUP_TEST_ROOT=str(root),
                   TEMP=str(temporary), TMP=str(temporary))
        subprocess.run([str(Path(setup).resolve()), "--silent", "--install-dir", str(installed)],
                       env=env, check=True, timeout=120)
        uninstaller = installed / "uninstall.exe"
        try:
            actual = subprocess.check_output([str(installed / "acecode.exe"), "--version"],
                                             env=env, text=True, timeout=30).strip()
            if actual != f"acecode v{version}":
                raise ValueError(f"Installed version mismatch: {actual}")
            record = json.loads((installed / ".acecode-install.json").read_text(encoding="utf-8"))
            if record.get("version") != version:
                raise ValueError("Installed record version mismatch")
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry) as key:
                if winreg.QueryValueEx(key, "DisplayVersion")[0] != version:
                    raise ValueError("Installed registry version mismatch")
        finally:
            if uninstaller.exists():
                subprocess.run([str(uninstaller), "--uninstall", "--silent"],
                               cwd=root, env=env, check=True, timeout=120)
        if installed.exists():
            raise ValueError("Uninstall left the installation directory")
        if sentinel.read_text(encoding="utf-8") != "preserve user data":
            raise ValueError("Uninstall changed user data")
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry):
                raise ValueError("Uninstall left its registry record")
        except FileNotFoundError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup", type=Path, required=True)
    parser.add_argument("--payload-zip", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--target", choices=MACHINES, required=True)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--install-volume", type=Path, help="Optional separate drive/directory for the install smoke")
    args = parser.parse_args()
    info = verify_setup(args.setup, args.payload_zip, args.version, args.target)
    if args.smoke:
        if args.target != "windows-x64" or pe_machine(Path(os.sys.executable).read_bytes()) != MACHINES[args.target]:
            parser.error("Lifecycle smoke requires a native x64 Python host and x64 setup")
        smoke_install(args.setup, args.version, args.install_volume)
    print(json.dumps({"setup": str(args.setup), "sha256": hashlib.sha256(args.setup.read_bytes()).hexdigest(),
                      "size": args.setup.stat().st_size, "smoke": args.smoke, **info}, indent=2))


if __name__ == "__main__":
    main()
