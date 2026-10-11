"""Verify that rebuilding after a Vite output change refreshes embedded bytes."""
from pathlib import Path
import gzip
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class EmbeddedAssetsRebuildTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cmake"), "requires CMake")
    def test_native_office_inlines_the_shared_character_source(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "pet.cpp"
            source = ROOT / 'assets/desktop_pet/agent_office_pet.html'
            shared = ROOT / 'assets/desktop_pet/office_characters.js'
            subprocess.run(['cmake', f'-DBIN2CPP_INPUT={source}', f'-DBIN2CPP_OUTPUT={output}',
                            '-DBIN2CPP_SYMBOL=desktop_pet_page', f'-DBIN2CPP_INLINE_JS={shared}',
                            '-P', str(ROOT / 'cmake/acecode_bin2cpp.cmake')], check=True, capture_output=True)
            decoded = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-f]{2})', output.read_text()))
            expected = source.read_bytes().replace(b'<script src="./office_characters.js"></script>',
                                                    b'<script>' + shared.read_bytes() + b'</script>')
            self.assertEqual(decoded, expected)
            self.assertNotIn(b'<script src=', decoded)

    def test_compressed_resources_round_trip_and_are_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            web = root / "web"
            web.mkdir()
            html = b'<html>__VERSION__\x1a\x00\xff' + b'large repeated frontend ' * 500 + b'</html>'
            (web / "index.html").write_bytes(html)
            (web / "empty.js").write_bytes(b'')
            (web / "small.bin").write_bytes(bytes(range(32)))
            output = root / "embedded.cpp"
            command = [sys.executable, str(ROOT / "scripts/embed_web_assets.py"),
                       "--root", str(web), "--output", str(output), "--version", "v-test"]
            subprocess.run(command, check=True, capture_output=True)
            generated = output.read_bytes()
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(output.read_bytes(), generated)
            text = generated.decode()
            arrays = dict(re.findall(r'unsigned char (asset_\d+)\[\] = \{(.*?)\};', text, re.S))
            entries = re.findall(r'\{"([^"]+)", \{(asset_\d+), (\d+), (\d+), (true|false)\}\}', text)
            self.assertEqual(len(entries), 3)
            for name, symbol, size, decoded_size, compressed in entries:
                data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', arrays[symbol]))
                self.assertEqual(len(data), int(size))
                if compressed == 'true':
                    data = gzip.decompress(data)
                expected = (web / name).read_bytes().replace(b'__VERSION__', b'v-test')
                self.assertEqual(data, expected)
                self.assertEqual(len(data), int(decoded_size))
                if name == 'index.html':
                    self.assertEqual(compressed, 'true')
                    self.assertLess(int(size), len(data) // 4)
                else:
                    self.assertEqual(compressed, 'false')

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "requires CMake and Ninja")
    def test_modified_asset_reconfigures_without_changing_its_filename(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            build = Path(directory) / "build"
            source.mkdir()
            web = source / "web"
            web.mkdir()
            index = web / "index.html"
            index.write_text("before", encoding="utf-8")
            module = (ROOT / "cmake/acecode_embed_assets.cmake").as_posix()
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.16)\nproject(embed_test NONE)\n'
                f'include("{module}")\n'
                'acecode_generate_embedded_assets("${CMAKE_SOURCE_DIR}/web" '
                '"${CMAKE_BINARY_DIR}/embedded.cpp" "test")\n', encoding="utf-8")

            def run(*args):
                result = subprocess.run(["cmake", *args], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            run("-S", str(source), "-B", str(build), "-G", "Ninja")
            generated = build / "embedded.cpp"
            self.assertIn("0x62,0x65,0x66,0x6f,0x72,0x65,", generated.read_text())
            index.write_text("after", encoding="utf-8")
            run("--build", str(build))
            self.assertIn("0x61,0x66,0x74,0x65,0x72,", generated.read_text())
            self.assertNotIn("0x62,0x65,0x66,0x6f,0x72,0x65,", generated.read_text())


if __name__ == "__main__":
    unittest.main()
