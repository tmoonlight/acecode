"""Build a validated, unsigned Microsoft Store MSIX from a current Windows build."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

REPO = Path(__file__).resolve().parents[3]
FOUNDATION = "http://schemas.microsoft.com/appx/manifest/foundation/windows10"
UAP = "http://schemas.microsoft.com/appx/manifest/uap/windows10"
UAP5 = UAP + "/5"
DESKTOP4 = "http://schemas.microsoft.com/appx/manifest/desktop/windows10/4"
RESCAP = FOUNDATION + "/restrictedcapabilities"
TEST_IDENTITY = {
    "name": "ACECode.LocalValidation",
    "publisher": "CN=ACECode Local Validation",
    "publisher_display_name": "ACECode Local Validation",
}
REQUIRED_BINARIES = (
    "acecode.exe", "acecode-desktop.exe", "acecode-computer-use.exe", "winpty-agent.exe",
)
MACHINES = {"x64": 0x8664, "arm64": 0xAA64}


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def store_version(version: str) -> str:
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("Project version must be a stable a.b.c version")
    major, minor, patch = map(int, version.split("."))
    if major >= 65535 or minor > 65535 or patch > 65535:
        raise ValueError("Project version exceeds MSIX version limits")
    return f"{major + 1}.{minor}.{patch}.0"


def load_identity(identity_file: Path | None, local_test: bool) -> dict[str, str]:
    if local_test:
        if identity_file:
            raise ValueError("--local-test cannot be combined with --identity-file")
        return dict(TEST_IDENTITY)
    if not identity_file:
        raise ValueError("Store builds require --identity-file with Partner Center identity")
    identity = json.loads(identity_file.read_text(encoding="utf-8-sig"))
    for field in TEST_IDENTITY:
        if not isinstance(identity.get(field), str) or not identity[field].strip():
            raise ValueError(f"Missing product identity: {field}")
        identity[field] = identity[field].strip()
    if not re.fullmatch(r"[A-Za-z0-9.-]{3,50}", identity["name"]):
        raise ValueError("Invalid Package/Identity/Name")
    if not identity["publisher"].startswith("CN="):
        raise ValueError("Publisher must be the exact certificate subject from Partner Center")
    if identity["name"] == TEST_IDENTITY["name"]:
        raise ValueError("Local validation identity cannot be used for Store builds")
    return identity


def verify_pe(path: Path, architecture: str) -> None:
    with path.open("rb") as stream:
        dos = stream.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise ValueError(f"Not a PE binary: {path}")
        offset = struct.unpack_from("<I", dos, 0x3C)[0]
        stream.seek(offset)
        header = stream.read(6)
    if len(header) != 6 or header[:4] != b"PE\0\0":
        raise ValueError(f"Invalid PE header: {path}")
    machine = struct.unpack_from("<H", header, 4)[0]
    if machine != MACHINES[architecture]:
        raise ValueError(f"Wrong architecture in {path.name}: 0x{machine:04x}, expected {architecture}")


def manifest(identity: dict[str, str], version: str, architecture: str,
             local_test: bool) -> bytes:
    for prefix, uri in (("", FOUNDATION), ("uap", UAP), ("uap5", UAP5),
                        ("desktop4", DESKTOP4), ("rescap", RESCAP)):
        ET.register_namespace(prefix, uri)
    def add(parent, name, attrs=None, text=None, namespace=FOUNDATION):
        item = ET.SubElement(parent, f"{{{namespace}}}{name}", attrs or {})
        item.text = text
        return item
    root = ET.Element(f"{{{FOUNDATION}}}Package", {
        "IgnorableNamespaces": "uap uap5 desktop4 rescap",
    })
    add(root, "Identity", {"Name": identity["name"], "Publisher": identity["publisher"],
                          "Version": version, "ProcessorArchitecture": architecture})
    display_name = "ACECode Local Validation" if local_test else "ACECode"
    props = add(root, "Properties")
    add(props, "DisplayName", text=display_name)
    add(props, "PublisherDisplayName", text=identity["publisher_display_name"])
    add(props, "Logo", text="Assets/StoreLogo.png")
    dependencies = add(root, "Dependencies")
    add(dependencies, "TargetDeviceFamily", {
        "Name": "Windows.Desktop", "MinVersion": "10.0.19041.0",
        "MaxVersionTested": "10.0.26100.0",
    })
    resources = add(root, "Resources")
    for language in ("en-us", "zh-cn"):
        add(resources, "Resource", {"Language": language})
    applications = add(root, "Applications")
    for app_id, executable in (("ACECode", "acecode-desktop.exe"), ("CLI", "acecode.exe")):
        app = add(applications, "Application", {
            "Id": app_id, "Executable": executable, "EntryPoint": "Windows.FullTrustApplication",
        })
        if app_id == "CLI":
            app.set(f"{{{DESKTOP4}}}SupportsMultipleInstances", "true")
        visual = {
            "DisplayName": display_name, "Description": "AI coding assistant for your projects",
            "Square150x150Logo": "Assets/Square150x150Logo.png",
            "Square44x44Logo": "Assets/Square44x44Logo.png", "BackgroundColor": "transparent",
        }
        if app_id == "CLI":
            visual["AppListEntry"] = "none"
        add(app, "VisualElements", visual, namespace=UAP)
        if app_id == "CLI":
            extensions = add(app, "Extensions")
            extension = add(extensions, "Extension", {
                "Category": "windows.appExecutionAlias", "Executable": executable,
                "EntryPoint": "Windows.FullTrustApplication",
            }, namespace=UAP5)
            aliases = add(extension, "AppExecutionAlias", {
                f"{{{DESKTOP4}}}Subsystem": "console",
            }, namespace=UAP5)
            # Local tests must not take over the production CLI alias.
            alias = "acecode-store-test.exe" if local_test else "acecode.exe"
            add(aliases, "ExecutionAlias", {"Alias": alias}, namespace=UAP5)
    capabilities = add(root, "Capabilities")
    add(capabilities, "Capability", {"Name": "runFullTrust"}, namespace=RESCAP)
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n"


def find_makeappx(explicit: Path | None) -> Path:
    if explicit:
        if not explicit.is_file():
            raise ValueError(f"Missing MakeAppx: {explicit}")
        return explicit.resolve()
    kits = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Windows Kits/10/bin"
    versions = sorted((p for p in kits.glob("10.*") if p.is_dir()),
                      key=lambda p: tuple(map(int, p.name.split("."))), reverse=True)
    for version in versions:
        candidate = version / "x64/makeappx.exe"
        if candidate.is_file():
            return candidate
    raise ValueError("Windows SDK MakeAppx.exe not found; pass --makeappx")


def copy_tree(source: Path, target: Path) -> None:
    if not source.is_dir():
        raise ValueError(f"Missing resources: {source}")
    for path in source.rglob("*"):
        if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
            raise ValueError(f"Resource links are not permitted: {path}")
    shutil.copytree(source, target)


def stage(build_dir: Path, configuration: str, payload: Path, architecture: str,
          version: str) -> None:
    binary_dir = build_dir / configuration
    if not (binary_dir / "acecode.exe").is_file():
        binary_dir = build_dir
    for name in REQUIRED_BINARIES:
        source = binary_dir / name
        if not source.is_file():
            raise ValueError(f"Missing required binary: {source}")
        verify_pe(source, architecture)
        shutil.copy2(source, payload / name)
    for source in sorted(binary_dir.glob("*.dll")):
        verify_pe(source, architecture)
        shutil.copy2(source, payload / source.name)
    probe = subprocess.run([str(binary_dir / "acecode.exe"), "--version"], check=True,
                           capture_output=True, text=True, encoding="utf-8", timeout=20)
    if not re.search(rf"(?<![\d.]){re.escape(version)}(?![\d.\w-])", probe.stdout):
        raise ValueError(f"Binary version does not match source {version}: {probe.stdout.strip()}")
    for name in ("LICENSE", "THIRD-PARTY-NOTICES", "README.md", "README_CN.md"):
        shutil.copy2(REPO / name, payload / name)
    share = payload / "share/acecode"
    share.mkdir(parents=True)
    copy_tree(REPO / "assets/models_dev", share / "models_dev")
    copy_tree(REPO / "assets/seed", share / "seed")
    subprocess.run([sys.executable, str(REPO / "scripts/verify_seed_bundle.py"),
                    "--source", str(REPO / "assets/seed"), "--packaged", str(share / "seed")],
                   check=True)


def make_assets(payload: Path) -> None:
    from PIL import Image
    asset_dir = payload / "Assets"
    asset_dir.mkdir()
    with Image.open(REPO / "assets/windows/acecode_icon.png") as original:
        original = original.convert("RGBA")
        for name, size in (("StoreLogo", 50), ("Square44x44Logo", 44),
                           ("Square150x150Logo", 150), ("StoreListing", 300)):
            original.resize((size, size), Image.Resampling.LANCZOS).save(asset_dir / f"{name}.png")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--architecture", choices=MACHINES, default="x64")
    parser.add_argument("--identity-file", type=Path)
    parser.add_argument("--local-test", action="store_true")
    parser.add_argument("--output-dir", type=Path, default=REPO / "build/store-msix")
    parser.add_argument("--makeappx", type=Path)
    args = parser.parse_args()
    try:
        identity = load_identity(args.identity_file, args.local_test)
        source = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(r"project\(acecode VERSION (\d+\.\d+\.\d+)", source)
        if not match:
            raise ValueError("Cannot determine project version")
        version = match[1]
        package_version = store_version(version)
        sdk = find_makeappx(args.makeappx)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        run_dir = Path(tempfile.mkdtemp(prefix="local-test-" if args.local_test else "store-",
                                       dir=args.output_dir.resolve()))
        payload = run_dir / "payload"
        payload.mkdir()
        stage(args.build_dir.resolve(), args.configuration, payload, args.architecture, version)
        make_assets(payload)
        (payload / "AppxManifest.xml").write_bytes(
            manifest(identity, package_version, args.architecture, args.local_test))
        suffix = "-LOCAL-TEST" if args.local_test else ""
        package = run_dir / f"ACECode-{package_version}-{args.architecture}{suffix}.msix"
        log_path = run_dir / "makeappx.log"
        with log_path.open("w", encoding="utf-8") as log:
            subprocess.run([str(sdk), "pack", "/d", str(payload), "/p", str(package),
                            "/h", "SHA256"], check=True, stdout=log, stderr=subprocess.STDOUT)
        inventory = {p.relative_to(payload).as_posix(): sha256(p)
                     for p in sorted(payload.rglob("*")) if p.is_file()}
        with zipfile.ZipFile(package) as archive:
            for name, digest in inventory.items():
                if hashlib.sha256(archive.read(name)).hexdigest() != digest:
                    raise ValueError(f"Packaged content mismatch: {name}")
        report = {
            "local_test_only": args.local_test, "identity": identity,
            "project_version": version, "package_version": package_version,
            "architecture": args.architecture, "package": str(package),
            "sha256": sha256(package), "size": package.stat().st_size,
            "source_commit": subprocess.check_output(
                ["git", "-C", str(REPO), "rev-parse", "HEAD"], text=True).strip(),
            "source_changes": subprocess.check_output(
                ["git", "-C", str(REPO), "status", "--short"], text=True).splitlines(),
            "files": inventory, "makeappx": str(sdk),
            "validation": {"makeappx": "passed", "archive_hashes": "passed",
                           "installed_runtime": "not_run", "store_certification": "not_submitted"},
        }
        (run_dir / "package-report.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"MSIX: {package}")
        print(f"SHA256: {report['sha256']}")
        print(f"Report: {run_dir / 'package-report.json'}")
        if args.local_test:
            print("LOCAL VALIDATION ONLY: this identity must not be submitted to Partner Center.")
        return 0
    except (ValueError, OSError, subprocess.SubprocessError, KeyError) as error:
        print(f"MSIX build failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
