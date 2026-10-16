#!/usr/bin/env python3
"""Offline contracts and mocked release flow; never invoke signing/notary tools."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / '.github/workflows/package.yml').read_text()

# All security-sensitive tools in temporary script copies are replaced below.
MOCK = r'''
import json, os, pathlib, shutil, sys
tool, *args = sys.argv[1:]
root = pathlib.Path(os.environ['TEST_ROOT'])
with (root / 'calls').open('a') as f: f.write(json.dumps([tool, *args]) + '\n')
def value(flag): return args[args.index(flag) + 1]
if tool == 'uname': print('Darwin')
elif tool == 'codesign':
    if '-dv' in args:
        print('TeamIdentifier=T52GZCH73Y\nAuthority=Developer ID Application: Test')
    if '--sign' in args and os.environ.get('MUTATE_PAYLOAD'):
        target = pathlib.Path(args[-1]) / 'Contents/Resources/ACECode.app/Contents/MacOS/ACECode'
        if target.exists(): target.write_text('mutated')
elif tool == 'plutil':
    if '-extract' in args:
        key = value('-extract')
        if key in ('status', 'id'): print(json.loads(pathlib.Path(args[-1]).read_text())[key])
        else: print({'CFBundleIdentifier':'dev.acecode.desktop', 'CFBundleShortVersionString':'1.2.3', 'CFBundleVersion':'123', 'CFBundleIconFile':'app.icns'}[key])
    elif '-create' in args: pathlib.Path(args[-1]).touch()
elif tool == 'ditto':
    source, target = map(pathlib.Path, args[-2:])
    if '-c' in args: target.write_text('zip')
    elif source.is_dir(): shutil.copytree(source, target, symlinks=True)
    else: shutil.copy2(source, target)
elif tool == 'hdiutil':
    counter = root / 'hdiutil-attempts'
    attempt = int(counter.read_text()) + 1 if counter.exists() else 1
    counter.write_text(str(attempt))
    if os.environ.get('HDIUTIL_FATAL'):
        print('hdiutil: create failed - Permission denied', file=sys.stderr)
        sys.exit(2)
    if attempt <= int(os.environ.get('HDIUTIL_BUSY_FAILURES', '0')):
        pathlib.Path(args[-1]).write_text('partial image')
        print('hdiutil: create failed - Resource busy', file=sys.stderr)
        sys.exit(1)
    assert value('-fs') == 'HFS+'
    image = pathlib.Path(value('-srcfolder'))
    assert [p.name for p in image.iterdir()] == ['ACECode Installer.app']
    pathlib.Path(args[-1]).write_text('dmg')
elif tool == 'xcrun':
    if '--show-sdk-path' in args: print('/mock-sdk')
    elif args[0] == 'swiftc': pathlib.Path(value('-o')).write_text('binary')
    elif args[:2] == ['notarytool', 'submit']:
        status = os.environ.get('NOTARY_STATUS', 'Accepted')
        if args[2].endswith('.dmg') and os.environ.get('REJECT_DMG'): status = 'Invalid'
        print(json.dumps({'status': status, 'id': 'test-submission'}))
    elif args[0] == 'stapler':
        counter = root / 'stapler-attempts'
        attempt = int(counter.read_text()) + 1 if counter.exists() else 1
        counter.write_text(str(attempt))
        if os.environ.get('STAPLER_FATAL'):
            print('The staple and validate action failed! Error 65.', file=sys.stderr)
            sys.exit(65)
        if attempt <= int(os.environ.get('STAPLER_TICKET_FAILURES', '0')):
            print('CloudKit: The network connection was lost. The validate action failed! Error 68.', file=sys.stderr)
            sys.exit(68)
elif tool == 'lipo':
    assert args[1] == '-verify_arch' and args[2] in ('x86_64', 'arm64')
    if os.environ.get('WRONG_ARCH'): sys.exit(1)
'''


class InstallerReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='installer release ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tools = self.root / 'tools'
        self.tools.mkdir()
        (self.tools / 'mock.py').write_text(MOCK)
        for tool in ('codesign', 'spctl', 'plutil', 'ditto', 'hdiutil', 'lipo', 'xcrun', 'uname'):
            path = self.tools / tool
            path.write_text('#!/bin/bash\nexec python3 "$(dirname "$0")/mock.py" ' + tool + ' "$@"\n')
            path.chmod(0o755)
        for name in ('scripts/macos_create_installer_dmg.sh', 'scripts/macos_stapler.sh',
                     'packaging/macos-installer/build.sh'):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            text = (ROOT / name).read_text()
            for tool in ('codesign', 'plutil', 'ditto', 'hdiutil', 'lipo'):
                text = text.replace('/usr/bin/' + tool, '"' + str(self.tools / tool) + '"')
            text = text.replace('/usr/sbin/spctl', '"' + str(self.tools / 'spctl') + '"')
            target.write_text(text)
        self.app = self.root / 'payload/ACECode.app'
        (self.app / 'Contents/MacOS').mkdir(parents=True)
        (self.app / 'Contents/Resources').mkdir()
        for name in ('MacOS/ACECode', 'MacOS/acecode-daemon', 'Resources/app.icns', 'Info.plist'):
            (self.app / 'Contents' / name).write_text('original')
        self.output = self.root / 'output/ACECode-1.2.3-macos-x64.dmg'
        self.env = dict(os.environ, TEST_ROOT=str(self.root), PATH=str(self.tools) + os.pathsep + os.environ['PATH'],
                        ACECODE_STAPLER_RETRY_DELAY='0')

    def run_pipeline(self, success=True, arch='x64'):
        result = subprocess.run(['bash', str(self.root / 'scripts/macos_create_installer_dmg.sh'),
                                 '--app', str(self.app), '--output', str(self.output), '--arch', arch,
                                 '--identity', 'test-fingerprint', '--keychain', '/mock-keychain',
                                 '--keychain-profile', 'offline-test'], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return [json.loads(line) for line in (self.root / 'calls').read_text().splitlines()]

    def test_release_flow_both_architectures(self):
        for arch, target in [('x64', 'x86_64'), ('arm64', 'arm64')]:
            self.output = self.root / f'{arch}.dmg'
            calls = self.run_pipeline(arch=arch)
            self.assertTrue(self.output.is_file())
            compile_call = [c for c in calls if c[:2] == ['xcrun', 'swiftc']][-1]
            self.assertIn(target + '-apple-macosx11.0', compile_call)
            signs = [c for c in calls if c[0] == 'codesign' and '--sign' in c]
            self.assertTrue(all('--deep' not in c for c in signs))
            self.assertTrue(all('/mock-keychain' in c for c in signs))
            self.assertIn('runtime', signs[-2])
            self.assertTrue(signs[-2][-1].endswith('ACECode Installer.app'))
            self.assertTrue(signs[-1][-1].endswith('release.dmg'))
            submissions = [c for c in calls if c[:3] == ['xcrun', 'notarytool', 'submit']]
            self.assertTrue(submissions[-2][3].endswith('installer.zip'))
            self.assertTrue(submissions[-1][3].endswith('release.dmg'))
            self.assertTrue(all('--wait' in c for c in submissions))
            self.assertEqual((self.app / 'Contents/MacOS/ACECode').read_text(), 'original')

    def test_rejected_notarization_never_creates_dmg(self):
        self.env['NOTARY_STATUS'] = 'Invalid'
        calls = self.run_pipeline(success=False)
        self.assertFalse(self.output.exists())
        self.assertFalse(any(c[0] == 'hdiutil' for c in calls))
        self.assertTrue(any(c[:3] == ['xcrun', 'notarytool', 'log'] for c in calls))

    def test_wrong_architecture_fails_before_signing(self):
        self.env['WRONG_ARCH'] = '1'
        calls = self.run_pipeline(success=False)
        self.assertFalse(any('--sign' in c for c in calls))

    def test_rejected_dmg_notarization_never_publishes(self):
        self.env['REJECT_DMG'] = '1'
        calls = self.run_pipeline(success=False)
        self.assertTrue(any(c[0] == 'hdiutil' for c in calls))
        self.assertFalse(self.output.exists())

    def test_transient_image_busy_retries_with_fresh_output(self):
        self.env['HDIUTIL_BUSY_FAILURES'] = '2'
        calls = self.run_pipeline()
        images = [c[-1] for c in calls if c[0] == 'hdiutil']
        self.assertEqual(len(images), 3)
        self.assertEqual(len(set(images)), 3)
        self.assertEqual(self.output.read_text(), 'dmg')

    def test_persistent_image_busy_stops_without_publishing(self):
        self.env['HDIUTIL_BUSY_FAILURES'] = '9'
        calls = self.run_pipeline(success=False)
        self.assertEqual(sum(c[0] == 'hdiutil' for c in calls), 3)
        self.assertFalse(self.output.exists())
        self.assertFalse(any(c[:3] == ['xcrun', 'notarytool', 'submit'] and c[3].endswith('.dmg') for c in calls))

    def test_image_errors_other_than_busy_are_not_retried(self):
        self.env['HDIUTIL_FATAL'] = '1'
        calls = self.run_pipeline(success=False)
        self.assertEqual(sum(c[0] == 'hdiutil' for c in calls), 1)
        self.assertFalse(self.output.exists())

    def test_ticket_service_disconnects_are_retried(self):
        # 场景:staple/validate 时连 Apple 票据服务(CloudKit)断线,stapler 退出码 68。
        # v1.0.7 的 macos-x64 正是这样整个 job 失败,重跑三次每次断在不同步骤(v1.0.6 也遇到过)。
        # 期望:同一条 stapler 命令原样重试,恢复后流水线照常产出 DMG。
        self.env['STAPLER_TICKET_FAILURES'] = '2'
        calls = self.run_pipeline()
        staples = [c for c in calls if c[:2] == ['xcrun', 'stapler']]
        self.assertGreaterEqual(len(staples), 3)
        self.assertEqual(staples[0], staples[1])
        self.assertEqual(staples[1], staples[2])
        self.assertEqual(self.output.read_text(), 'dmg')

    def test_persistent_ticket_service_errors_stop_without_publishing(self):
        # 场景:票据服务一直不可用。期望:重试满 ACECODE_STAPLER_ATTEMPTS 次后失败,
        # 不再继续公证或产出 DMG(第一处 stapler 在构建安装器时校验原始应用)。
        self.env['STAPLER_TICKET_FAILURES'] = '99'
        self.env['ACECODE_STAPLER_ATTEMPTS'] = '3'
        calls = self.run_pipeline(success=False)
        staples = [c for c in calls if c[:2] == ['xcrun', 'stapler']]
        self.assertEqual(len(staples), 3)
        self.assertEqual(len({json.dumps(c) for c in staples}), 1)
        self.assertFalse(any(c[:3] == ['xcrun', 'notarytool', 'submit'] for c in calls))
        self.assertFalse(self.output.exists())

    def test_stapler_errors_other_than_ticket_service_are_not_retried(self):
        # 场景:stapler 因票据缺失、签名无效等原因失败(非 68)。
        # 期望:不重试,立即失败,避免把真正的签名问题拖成长时间等待。
        self.env['STAPLER_FATAL'] = '1'
        calls = self.run_pipeline(success=False)
        self.assertEqual(sum(c[:2] == ['xcrun', 'stapler'] for c in calls), 1)
        self.assertFalse(self.output.exists())

    def test_cli_validation_without_tools(self):
        for script in ('packaging/macos-installer/build.sh', 'scripts/macos_create_installer_dmg.sh'):
            for args, status in [(['--help'], 0), ([], 2), (['--arch'], 2), (['--unknown'], 2)]:
                result = subprocess.run(['bash', str(ROOT / script), *args], capture_output=True)
                self.assertEqual(result.returncode, status)

    def test_resigned_or_modified_payload_fails_before_notarization(self):
        self.env['MUTATE_PAYLOAD'] = '1'
        calls = self.run_pipeline(success=False)
        self.assertFalse(any('notarytool' in c for c in calls))

    def test_workflow_order_and_gate(self):
        step = WORKFLOW.split('- name: Build signed and notarized macOS installer DMG', 1)[1].split('- name:', 1)[0]
        self.assertIn("steps.macos-release.outputs.enabled == 'true'", step)
        self.assertNotIn('pkg_enabled', step)
        self.assertIn('--app "build/ACECode.app"', step)
        self.assertIn('--arch "${{ matrix.installer_arch }}"', step)
        self.assertLess(WORKFLOW.index('- name: Notarize and staple macOS app'), WORKFLOW.index('- name: Build signed'))
        self.assertLess(WORKFLOW.index('- name: Build signed'), WORKFLOW.index('- name: Clean up macOS signing material'))
        self.assertLess(WORKFLOW.index('- name: Notarize and validate macOS PKG'), WORKFLOW.index('- name: Upload macOS PKG'))
        self.assertLess(WORKFLOW.index('- name: Upload macOS PKG'), WORKFLOW.index('- name: Build signed'))
        self.assertIn('installer_arch: x64', WORKFLOW)
        self.assertIn('installer_arch: arm64', WORKFLOW)

    def test_dmg_release_naming_and_count(self):
        block = WORKFLOW.split('      - name: Collect release assets', 1)[1].split('      - name:', 1)[0]
        block = textwrap.dedent(block.split('        run: |\n', 1)[1])
        # Exercise the real DMG guard, not a reimplementation of it.
        guard = block[block.index('dmg_count='):block.index('unsigned_pkg=')]
        artifacts = self.root / 'artifacts'
        artifacts.mkdir()
        def check(names, success):
            for p in artifacts.rglob('*'):
                if p.is_file(): p.unlink()
            for name in names:
                path = artifacts / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            result = subprocess.run(['bash', '-eu', '-c', 'release_version=1.2.3\n' + guard], cwd=self.root, capture_output=True)
            self.assertEqual(result.returncode == 0, success, result.stderr)
        valid = ['ACECode-1.2.3-macos-x64.dmg', 'ACECode-1.2.3-macos-arm64.dmg']
        check(valid, True)
        check([], False)
        check(valid[:1], False)
        check(valid + ['extra.dmg'], False)
        check([valid[0], 'old.dmg'], False)
        check([valid[0], 'nested/' + valid[0]], False)
        check([valid[0], 'ACECode-1.2.3-macos-arm64-unsigned.dmg'], False)

    def test_tagged_release_requires_installer_credentials(self):
        lines = WORKFLOW.splitlines()
        start = lines.index('          application_missing=()')
        end = next(index for index in range(start, len(lines))
                   if lines[index].startswith('      - name: Configure MSVC'))
        guard = textwrap.dedent('\n'.join(lines[start:end]))
        output = self.root / 'github-output'
        environment = dict(self.env, GITHUB_REF='refs/tags/v1.2.3', GITHUB_OUTPUT=str(output))
        for name in ('MACOS_CERTIFICATE_BASE64', 'MACOS_CERTIFICATE_PASSWORD',
                     'APPLE_ID', 'APPLE_TEAM_ID', 'APPLE_APP_SPECIFIC_PASSWORD'):
            environment[name] = 'fixture'
        bash = os.environ.get('ACECODE_TEST_BASH', 'bash')
        for certificate, password, success in (('', '', False), ('fixture', '', False),
                                               ('', 'fixture', False), ('fixture', 'fixture', True)):
            with self.subTest(certificate=bool(certificate), password=bool(password)):
                output.unlink(missing_ok=True)
                environment.update(MACOS_INSTALLER_CERTIFICATE_BASE64=certificate,
                                   MACOS_INSTALLER_CERTIFICATE_PASSWORD=password)
                result = subprocess.run([bash, '-eu', '-c', guard], env=environment, capture_output=True)
                self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
                if success:
                    self.assertIn('pkg_enabled=true', output.read_text())
                else:
                    self.assertNotIn('pkg_enabled=true', output.read_text() if output.exists() else '')

    def test_updater_selection_excludes_installers(self):
        spec = importlib.util.spec_from_file_location('selector', ROOT / 'scripts/select_macos_update_assets.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for arch in ('x64', 'arm64'):
            for suffix in ('.dmg', '.pkg', '-update.zip', '-update-unsigned.zip'):
                (self.root / f'ACECode-1.2.3-macos-{arch}{suffix}').touch()
        selected = module.select_assets(self.root, '1.2.3')
        self.assertEqual(len(selected), 2)
        self.assertTrue(all(p.name.endswith('-update.zip') for p in selected))
        selected[0].unlink()
        with self.assertRaises(ValueError): module.select_assets(self.root, '1.2.3')


if __name__ == '__main__':
    unittest.main()
