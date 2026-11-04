import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("setup_verification", ROOT / "scripts/verify_windows_setup.py")
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


def pe(machine=0x8664):
    data = bytearray(80)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, machine)
    return bytes(data)


def fixture(target="windows-x64"):
    stub = pe(verify.MACHINES[target])
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name in ("acecode.exe", "acecode-desktop.exe", "acecode-computer-use.exe"):
            archive.writestr(name, stub)
    payload = buffer.getvalue()
    info = verify.packager.payload_info("1.2.3", target, payload, stub)
    resources = {"ACECODE_PAYLOAD": payload, "ACECODE_UNINSTALLER": stub,
                 "ACECODE_PAYLOAD_INFO": json.dumps(info).encode()}
    return stub, resources, payload


class SetupReleaseTest(unittest.TestCase):
    def test_both_architectures_accept_exact_payload(self):
        for target in verify.MACHINES:
            stub, resources, payload = fixture(target)
            self.assertEqual(verify.verify_contents(stub, resources, payload, "1.2.3", target)["target"], target)

    def test_version_architecture_and_payload_must_match_release(self):
        stub, resources, payload = fixture()
        for version, target, expected in (("1.2.4", "windows-x64", payload),
                                          ("1.2.3", "windows-arm64", payload),
                                          ("1.2.3", "windows-x64", payload + b"changed")):
            with self.subTest(version=version, target=target), self.assertRaises(ValueError):
                verify.verify_contents(stub, resources, expected, version, target)

    def test_altered_payload_or_uninstaller_is_rejected(self):
        for name in ("ACECODE_PAYLOAD", "ACECODE_UNINSTALLER"):
            stub, resources, payload = fixture()
            resources[name] += b"changed"
            with self.subTest(resource=name), self.assertRaises(ValueError):
                verify.verify_contents(stub, resources, payload, "1.2.3", "windows-x64")

    def test_uninstaller_architecture_checked_even_with_matching_hash(self):
        stub, resources, payload = fixture()
        resources["ACECODE_UNINSTALLER"] = pe(0xAA64)
        info = json.loads(resources["ACECODE_PAYLOAD_INFO"])
        info["uninstaller_sha256"] = hashlib.sha256(resources["ACECODE_UNINSTALLER"]).hexdigest()
        resources["ACECODE_PAYLOAD_INFO"] = json.dumps(info).encode()
        with self.assertRaisesRegex(ValueError, "Uninstaller architecture"):
            verify.verify_contents(stub, resources, payload, "1.2.3", "windows-x64")

    def test_invalid_pe_rejected(self):
        for data in (b"", b"MZ", b"MZ" + b"\xff" * 100):
            with self.assertRaises(ValueError):
                verify.pe_machine(data)

    @unittest.skipUnless(os.name == "nt", "Windows resource loader")
    def test_embedded_resources_are_read_back_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as temp:
            stub = Path(sys.executable)
            _, _, payload = fixture()
            out = Path(temp) / "setup.exe"
            info = verify.packager.build_setup(stub, payload, "1.2.3", "windows-x64", out)
            actual = verify.read_resources(out)
            self.assertEqual(actual["ACECODE_PAYLOAD"], payload)
            self.assertEqual(actual["ACECODE_UNINSTALLER"], stub.read_bytes())
            self.assertEqual(json.loads(actual["ACECODE_PAYLOAD_INFO"]), info)


if __name__ == "__main__":
    unittest.main()
