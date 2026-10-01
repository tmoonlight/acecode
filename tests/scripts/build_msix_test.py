import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "build_msix", ROOT / "installer/windows/store/build_msix.py")
msix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(msix)


class BuildMsixTest(unittest.TestCase):
    def test_store_identity_is_required_and_test_mode_is_explicit(self):
        with self.assertRaisesRegex(ValueError, "identity-file"):
            msix.load_identity(None, False)
        self.assertEqual(msix.load_identity(None, True), msix.TEST_IDENTITY)
        with self.assertRaises(ValueError):
            msix.load_identity(Path("identity.json"), True)

    def test_identity_validation_and_xml_escaping(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "identity.json"
            identity = dict(msix.TEST_IDENTITY, name="12345.Publisher.ACECode",
                            publisher='CN=Publisher & Company',
                            publisher_display_name="Publisher & Company")
            path.write_text(json.dumps(identity))
            result = msix.load_identity(path, False)
            root = ET.fromstring(msix.manifest(result, "1.9.30.0", "x64", False))
            self.assertEqual(root.find(f"{{{msix.FOUNDATION}}}Identity").get("Publisher"),
                             identity["publisher"])
            del identity["publisher"]
            path.write_text(json.dumps(identity))
            with self.assertRaisesRegex(ValueError, "publisher"):
                msix.load_identity(path, False)

    def test_version_mapping_is_monotonic_and_store_valid(self):
        self.assertEqual(msix.store_version("0.9.30"), "1.9.30.0")
        self.assertEqual(msix.store_version("1.0.0"), "2.0.0.0")
        for invalid in ("0.9.30-pre.1", "0.9", "1.65536.0", "65535.0.0", "-1.0.0"):
            with self.assertRaises(ValueError, msg=invalid):
                msix.store_version(invalid)

    def test_console_alias_and_full_trust_entries(self):
        for local, alias in ((False, "acecode.exe"), (True, "acecode-store-test.exe")):
            root = ET.fromstring(msix.manifest(msix.TEST_IDENTITY, "1.9.30.0", "x64", local))
            apps = root.findall(f".//{{{msix.FOUNDATION}}}Application")
            self.assertEqual([app.get("Executable") for app in apps],
                             ["acecode-desktop.exe", "acecode.exe"])
            self.assertTrue(all(app.get("EntryPoint") == "Windows.FullTrustApplication" for app in apps))
            self.assertEqual(apps[1].get(f"{{{msix.DESKTOP4}}}SupportsMultipleInstances"), "true")
            execution = root.find(f".//{{{msix.UAP5}}}AppExecutionAlias")
            self.assertEqual(execution.get(f"{{{msix.DESKTOP4}}}Subsystem"), "console")
            self.assertEqual(execution[0].get("Alias"), alias)
            self.assertEqual(root.find(f".//{{{msix.RESCAP}}}Capability").get("Name"), "runFullTrust")

    def test_wrong_architecture_and_invalid_pe_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "app.exe"
            raw = bytearray(70)
            raw[:2] = b"MZ"
            struct.pack_into("<I", raw, 0x3C, 64)
            raw[64:68] = b"PE\0\0"
            struct.pack_into("<H", raw, 68, 0x8664)
            path.write_bytes(raw)
            msix.verify_pe(path, "x64")
            with self.assertRaisesRegex(ValueError, "architecture"):
                msix.verify_pe(path, "arm64")
            path.write_bytes(b"not an executable")
            with self.assertRaisesRegex(ValueError, "PE"):
                msix.verify_pe(path, "x64")


if __name__ == "__main__":
    unittest.main()
