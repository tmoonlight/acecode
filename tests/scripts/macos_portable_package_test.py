#!/usr/bin/env python3
"""Run portable packaging against deterministic tools, without a Mac or build."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASH = shutil.which('bash')
if sys.platform == 'win32':
    git = shutil.which('git')
    candidate = Path(git).resolve().parents[1] / 'bin' / 'bash.exe' if git else None
    BASH = str(candidate) if candidate and candidate.is_file() else None

MOCK_TOOL = r'''
import hashlib, json, os, pathlib, shutil, sys, zipfile
root = pathlib.Path(os.environ['FIXTURE_ROOT'])
kind, *args = sys.argv[1:]
with (root / 'calls.jsonl').open('a') as log:
    log.write(json.dumps([kind, *args]) + '\n')
def value(flag):
    return args[args.index(flag) + 1]
if kind == 'cmake':
    if '--version' in args:
        print('cmake version mock')
    elif '-S' in args:
        build = pathlib.Path(value('-B'))
        (build / 'generated').mkdir(parents=True, exist_ok=True)
        arch = next(a.split('=', 1)[1] for a in args if a.startswith('-DCMAKE_OSX_ARCHITECTURES='))
        triplet = next(a.split('=', 1)[1] for a in args if a.startswith('-DVCPKG_TARGET_TRIPLET='))
        (build / 'CMakeCache.txt').write_text('CMAKE_OSX_ARCHITECTURES:STRING=' + arch + '\nVCPKG_TARGET_TRIPLET:STRING=' + triplet + '\n')
        (build / 'generated/static_assets_data.cpp').write_text('embedded')
    elif '--build' in args:
        build = pathlib.Path(value('--build'))
        arch = (build / 'CMakeCache.txt').read_text().split('=', 1)[1].splitlines()[0]
        arch = os.environ.get('WRONG_BINARY_ARCH', arch)
        for relative in ('acecode', 'acecode-computer-use', 'acecode-ios-sim', 'ACECode.app/Contents/MacOS/ACECode', 'ACECode.app/Contents/MacOS/acecode-daemon', 'ACECode.app/Contents/MacOS/acecode-computer-use', 'ACECode.app/Contents/MacOS/acecode-ios-sim'):
            binary = build / relative
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_text('#!/bin/sh\n# provider-logos arch=' + arch + ' ' + (root / 'backend.txt').read_text())
            binary.chmod(0o755)
        resources = build / 'ACECode.app/Contents/Resources/share/acecode'
        shutil.copytree(root / 'assets/models_dev', resources / 'models_dev', dirs_exist_ok=True)
        shutil.copytree(root / 'assets/seed', resources / 'seed', dirs_exist_ok=True)
    elif '--install' in args:
        if value('--component') == 'computer_use_runtime':
            shutil.copy2(pathlib.Path(value('--install')) / 'acecode-computer-use', pathlib.Path(value('--prefix')) / 'acecode-computer-use')
        elif value('--component') == 'ios_simulator_runtime':
            shutil.copy2(pathlib.Path(value('--install')) / 'acecode-ios-sim', pathlib.Path(value('--prefix')) / 'acecode-ios-sim')
        else:
            resource = 'models_dev' if value('--component') == 'models_dev_registry' else 'seed'
            shutil.copytree(root / 'assets' / resource, pathlib.Path(value('--prefix')) / 'share/acecode' / resource, dirs_exist_ok=True)
elif kind == 'lipo':
    assert args[1] == '-verify_arch'
    sys.exit(0 if 'arch=' + args[2] + ' ' in pathlib.Path(args[0]).read_text() else 1)
elif kind == 'ditto':
    if '-c' in args:
        source, target = map(pathlib.Path, args[-2:])
        with zipfile.ZipFile(target, 'w') as archive:
            for path in source.rglob('*'):
                if path.is_file():
                    archive.write(path, path.relative_to(source.parent))
    else:
        source, target = map(pathlib.Path, args[-2:])
        with zipfile.ZipFile(source) as archive:
            archive.extractall(target)
            for entry in archive.infolist():
                (target / entry.filename).chmod(0o755)
elif kind == 'shasum':
    print(hashlib.sha256(pathlib.Path(args[-1]).read_bytes()).hexdigest(), args[-1])
elif kind == 'git':
    print('fixture')
'''


@unittest.skipUnless(BASH, 'Bash required (Git Bash on Windows)')
class PortablePackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='acecode-portable-test-')
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / 'repo'
        self.repo.mkdir()
        for directory in ('scripts', 'web/src', 'web/dist', 'assets/models_dev', 'assets/seed', 'tools'):
            (self.repo / directory).mkdir(parents=True, exist_ok=True)
        def write(relative, text):
            (self.repo / relative).write_text(text, encoding='utf-8', newline='\n')
        write('scripts/macos_create_portable_zip.sh', (ROOT / 'scripts/macos_create_portable_zip.sh').read_text(encoding='utf-8'))
        write('scripts/verify_seed_bundle.py', 'import pathlib, sys\nassert (pathlib.Path(sys.argv[-1]) / "fixture.txt").is_file()\n')
        write('CMakeLists.txt', 'project(acecode VERSION 1.2.3)\n')
        write('README.md', 'readme')
        write('README_CN.md', 'readme')
        write('backend.txt', 'backend-v1')
        write('web/dist/index.html', 'provider-logos')
        write('assets/seed/fixture.txt', 'seed')
        for name in ('api.json', 'MANIFEST.json', 'LICENSE'):
            write('assets/models_dev/' + name, name)
        write('tools/mock.py', MOCK_TOOL)
        for kind in ('cmake', 'lipo', 'ditto', 'shasum', 'git'):
            write('tools/' + kind, '#!/usr/bin/env bash\nexec python3 "$(dirname "$0")/mock.py" ' + kind + ' "$@"\n')
            (self.repo / 'tools' / kind).chmod(0o755)
        self.env = {**os.environ, 'FIXTURE_ROOT': self.repo.as_posix(),
                    'PATH': str(self.repo / 'tools') + os.pathsep + os.environ['PATH'],
                    'CMAKE_BIN': (self.repo / 'tools/cmake').as_posix(),
                    'DITTO_BIN': (self.repo / 'tools/ditto').as_posix(),
                    'LIPO_BIN': (self.repo / 'tools/lipo').as_posix()}

    def run_package(self, *args, success=True):
        result = subprocess.run([BASH, str(self.repo / 'scripts/macos_create_portable_zip.sh'), *args],
                                cwd=self.temp.name, env=self.env, capture_output=True, text=True,
                                encoding='utf-8', errors='replace')
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def calls(self):
        path = self.repo / 'calls.jsonl'
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def test_backend_only_changes_rebuild_and_external_output_is_honored(self):
        output = Path(self.temp.name) / 'external output' / 'custom.zip'
        self.run_package('--arch', 'arm64', '--output', str(output))
        self.assertTrue(output.is_file())
        shutil.rmtree(self.repo / 'dist/acecode-macos-arm64')
        (self.repo / 'backend.txt').write_text('backend-v2')
        self.run_package('--arch', 'arm64', '--output', str(output))
        calls = self.calls()
        self.assertEqual(sum(c[0] == 'cmake' and '-S' in c for c in calls), 1)
        self.assertEqual(sum(c[0] == 'cmake' and '--build' in c for c in calls), 2)
        configure = next(c for c in calls if c[0] == 'cmake' and '-S' in c)
        self.assertIn('-DVCPKG_TARGET_TRIPLET=arm64-osx', configure)
        self.assertIn('macos-arm64-release', configure[configure.index('-B') + 1])
        self.assertEqual(sum(c[0] == 'lipo' for c in calls), 14)
        import zipfile
        with zipfile.ZipFile(output) as archive:
            self.assertIn(b'backend-v2', archive.read('acecode-macos-arm64/acecode'))

    def test_x64_uses_separate_build_and_triplet(self):
        self.run_package('--arch', 'x64')
        configure = next(c for c in self.calls() if c[0] == 'cmake' and '-S' in c)
        self.assertIn('-DVCPKG_TARGET_TRIPLET=x64-osx', configure)
        self.assertIn('-DCMAKE_OSX_ARCHITECTURES=x86_64', configure)
        self.assertIn('macos-x64-release', configure[configure.index('-B') + 1])

    def test_incompatible_cache_is_rejected_before_build(self):
        build = self.repo / 'existing'
        build.mkdir()
        (build / 'CMakeCache.txt').write_text('CMAKE_OSX_ARCHITECTURES:STRING=x86_64\nVCPKG_TARGET_TRIPLET:STRING=x64-osx\n')
        result = self.run_package('--arch', 'arm64', '--build-dir', str(build), success=False)
        self.assertIn('cache architecture/triplet does not match', result.stderr)
        self.assertEqual(self.calls(), [])

    def test_wrong_binary_architecture_is_rejected_before_archive(self):
        self.env['WRONG_BINARY_ARCH'] = 'x86_64'
        result = self.run_package('--arch', 'arm64', success=False)
        self.assertIn('wrong architecture', result.stderr)
        self.assertFalse(any(c[0] == 'ditto' for c in self.calls()))


if __name__ == '__main__':
    unittest.main()
