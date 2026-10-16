#!/usr/bin/env python3
"""Exercise the real CLI forwarder and update script with offline Apple tool fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
MOCK_DITTO = '''#!/usr/bin/env python3
import os, pathlib, shutil, sys, zipfile
args = sys.argv[1:]
source, target = map(pathlib.Path, args[-2:])
if '-c' in args:
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        for p in source.rglob('*'):
            archive.write(p, p.relative_to(source.parent))
elif '-x' in args:
    with zipfile.ZipFile(source) as archive:
        archive.extractall(target)
        for item in archive.infolist():
            os.chmod(target / item.filename, (item.external_attr >> 16) & 0o777)
elif source.is_dir(): shutil.copytree(source, target)
else: shutil.copy2(source, target)
'''


@unittest.skipUnless(os.name == 'posix' and shutil.which('c++') and shutil.which('zip'),
                     'requires POSIX C++ compiler and zip (macOS or Linux)')
class MacUpdateDedupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiled = tempfile.TemporaryDirectory(prefix='ace-cli-compiler-')
        cls.launcher = Path(cls.compiled.name) / 'acecode-cli'
        subprocess.run(['c++', '-std=c++17', '-Os',
                        str(ROOT / 'src/apps/desktop/macos_cli_launcher.cpp'),
                        '-o', str(cls.launcher)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.compiled.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ace update space-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'source/ACECode.app'
        self.macos = self.app / 'Contents/MacOS'
        self.macos.mkdir(parents=True)
        self.resources = self.app / 'Contents/Resources/share/acecode'
        self.resources.mkdir(parents=True)
        shutil.copytree(ROOT / 'assets/seed', self.resources / 'seed')
        shutil.copytree(ROOT / 'assets/models_dev', self.resources / 'models_dev')
        shutil.copy2(self.launcher, self.macos / 'acecode-cli')
        for name in ['ACECode', 'acecode-computer-use', 'acecode-ios-sim', 'acecode-daemon']:
            p = self.macos / name
            p.write_text('#!/usr/bin/env python3\nimport json, os, sys\n'
                         'print(json.dumps(sys.argv[1:]))\n'
                         'sys.exit(int(os.environ.get("FIXTURE_EXIT", "0")))\n')
            p.chmod(0o755)
        (self.app / 'Contents/Info.plist').write_text('<plist/>')
        tools = self.root / 'tools'
        tools.mkdir()
        ditto = tools / 'ditto'
        ditto.write_text(MOCK_DITTO)
        ditto.chmod(0o755)
        uname = tools / 'uname'
        uname.write_text('#!/bin/sh\nprintf Darwin\n')
        uname.chmod(0o755)
        scripts = self.root / 'scripts'
        scripts.mkdir()
        shutil.copytree(ROOT / 'assets/seed', self.root / 'assets/seed')
        shutil.copy2(ROOT / 'scripts/verify_seed_bundle.py', scripts / 'verify_seed_bundle.py')
        text = (ROOT / 'scripts/macos_create_update_zip.sh').read_text()
        text = text.replace('/usr/bin/ditto', '"' + str(ditto) + '"')
        self.script = scripts / 'macos_create_update_zip.sh'
        self.script.write_text(text)
        self.env = {**os.environ, 'PATH': str(tools) + os.pathsep + os.environ['PATH']}
        self.archive = self.root / 'update.zip'

    def package(self, success=True):
        result = subprocess.run(['bash', str(self.script), '--app', str(self.app),
                                 '--output', str(self.archive)], env=self.env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def extract(self):
        target = self.root / 'installed folder'
        with zipfile.ZipFile(self.archive) as archive:
            archive.extractall(target)
            for item in archive.infolist():
                (target / item.filename).chmod((item.external_attr >> 16) & 0o777)
        return target

    def test_single_runtime_payload_and_legacy_cli_entry(self):
        self.package()
        with zipfile.ZipFile(self.archive) as archive:
            names = archive.namelist()
            self.assertIn('acecode', names)
            self.assertEqual(sum(n.endswith('/acecode-daemon') for n in names), 1)
            self.assertEqual(sum(n.endswith('/models_dev/api.json') for n in names), 1)
            self.assertFalse(any(n.startswith(('share/', 'channels/')) for n in names))
            self.assertNotIn('acecode-computer-use', names)
            self.assertNotIn('acecode-ios-sim', names)
            self.assertEqual(sum(n.endswith('/MacOS/acecode-ios-sim') for n in names), 1)
            self.assertLess(archive.getinfo('acecode').file_size, 256 * 1024)
        installed = self.extract()
        args = ['two words', '', '中文路径', '--literal=$HOME;echo nope']
        result = subprocess.run([str(installed / 'acecode'), *args], cwd=self.root,
                                capture_output=True, text=True,
                                env={**self.env, 'FIXTURE_EXIT': '37'})
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertEqual(json.loads(result.stdout), args)

    def test_path_symlink_and_bundled_invocation(self):
        self.package()
        installed = self.extract()
        bin_dir = self.root / 'bin'
        bin_dir.mkdir()
        (bin_dir / 'acecode').symlink_to(installed / 'acecode')
        env = {**self.env, 'PATH': str(bin_dir) + os.pathsep + self.env['PATH']}
        for command in ['acecode', str(installed / 'ACECode.app/Contents/MacOS/acecode-cli')]:
            result = subprocess.run([command, '--version'], env=env, cwd=self.root,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), ['--version'])

    def test_missing_runtime_and_missing_or_oversized_forwarder(self):
        shutil.copy2(self.launcher, self.root / 'acecode')
        result = subprocess.run([str(self.root / 'acecode')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 127)
        self.assertIn('cannot start', result.stderr)
        (self.macos / 'acecode-cli').unlink()
        self.assertIn('missing the CLI forwarder', self.package(False).stderr)
        (self.macos / 'acecode-cli').write_bytes(b'x' * (262144 + 1))
        (self.macos / 'acecode-cli').chmod(0o755)
        self.assertIn('256 KiB', self.package(False).stderr)


if __name__ == '__main__':
    unittest.main()
