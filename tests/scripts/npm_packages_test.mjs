import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const platforms = ['linux-x64', 'linux-arm64', 'windows-x64', 'windows-arm64', 'macos-x64', 'macos-arm64'];
const assets = ['bridge.mjs', 'protocol.mjs', 'package.json', 'package-lock.json'];

function write(root, relative, value = 'fixture') {
  const file = path.join(root, relative);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, value);
}

function resources(root) {
  for (const file of ['LICENSE', 'MANIFEST.json', 'api.json']) {
    write(root, `share/acecode/models_dev/${file}`);
  }
  for (const file of assets) write(root, `channels/whatsapp/${file}`, `channel:${file}`);
}

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ace-npm-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const input = path.join(root, 'input');
  const output = path.join(root, 'output');
  for (const platform of platforms) {
    const dir = path.join(input, `acecode-${platform}`);
    resources(dir);
    write(dir, platform.startsWith('windows') ? 'acecode.exe' : 'acecode');
    if (platform.startsWith('windows')) write(dir, 'acecode-computer-use.exe', 'computer-use-runtime');
    if (platform.startsWith('macos')) {
      write(dir, 'acecode-computer-use', 'computer-use-runtime');
      write(dir, 'acecode-ios-sim', 'ios-simulator-runtime');
      write(dir, 'ACECode.app/Contents/MacOS/ACECode');
      write(dir, 'ACECode.app/Contents/MacOS/acecode-computer-use', 'computer-use-runtime');
      write(dir, 'ACECode.app/Contents/MacOS/acecode-ios-sim', 'ios-simulator-runtime');
      resources(path.join(dir, 'ACECode.app/Contents/Resources'));
    } else {
      write(dir, platform.startsWith('windows') ? 'acecode-desktop.exe' : 'acecode-desktop');
      if (platform.startsWith('linux')) write(dir, 'acecode-logo.png');
    }
  }
  return { input, output, run: () => spawnSync(process.execPath, [
    path.join(repo, 'scripts/npm/prepare-npm-packages.mjs'),
    '--version', '0.9.20', '--input', input, '--output', output,
  ], { encoding: 'utf8' }) };
}

test('all six npm platforms preserve bridge assets and exclude runtime state', (t) => {
  const { input, output, run } = fixture(t);
  write(input, 'acecode-windows-x64/channels/whatsapp/node_modules/private/index.js');
  write(input, 'acecode-windows-x64/channels/whatsapp/auth/creds.json');
  const result = run();
  assert.equal(result.status, 0, result.stderr);
  for (const platform of platforms) {
    const npmPlatform = platform.replace('windows-', 'win32-').replace('macos-', 'darwin-');
    const helper = path.join(output, 'platform', npmPlatform, 'acecode-computer-use.exe');
    if (platform.startsWith('windows')) {
      assert.equal(fs.readFileSync(helper, 'utf8'), 'computer-use-runtime');
    } else {
      assert.equal(fs.existsSync(helper), false);
    }
    if (platform.startsWith('macos')) {
      const macHelper = path.join(output, 'platform', npmPlatform, 'acecode-computer-use');
      assert.equal(fs.readFileSync(macHelper, 'utf8'), 'computer-use-runtime');
      assert.notEqual(fs.statSync(macHelper).mode & 0o111, 0);
      assert.equal(fs.readFileSync(path.join(output, 'platform', npmPlatform,
        'ACECode.app/Contents/MacOS/acecode-computer-use'), 'utf8'), 'computer-use-runtime');
      const simulator = path.join(output, 'platform', npmPlatform, 'acecode-ios-sim');
      assert.equal(fs.readFileSync(simulator, 'utf8'), 'ios-simulator-runtime');
      assert.notEqual(fs.statSync(simulator).mode & 0o111, 0);
    } else {
      // The iOS Simulator helper is a macOS-only payload.
      assert.equal(fs.existsSync(path.join(output, 'platform', npmPlatform, 'acecode-ios-sim')), false);
    }
    const channel = path.join(output, 'platform', npmPlatform, 'channels/whatsapp');
    assert.deepEqual(fs.readdirSync(channel).sort(), [...assets].sort());
    for (const file of assets) assert.equal(fs.readFileSync(path.join(channel, file), 'utf8'), `channel:${file}`);
  }
  const cli = JSON.parse(fs.readFileSync(path.join(output, 'cli/package.json'), 'utf8'));
  assert.equal(Object.keys(cli.optionalDependencies).length, 6);
});

test('missing lockfile rejects a platform package before publication', (t) => {
  const { input, run } = fixture(t);
  fs.unlinkSync(path.join(input, 'acecode-linux-arm64/channels/whatsapp/package-lock.json'));
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /linux-arm64.*missing WhatsApp bridge asset: package-lock.json/);
});

test('missing Windows Computer Use runtime rejects npm packaging', (t) => {
  const { input, run } = fixture(t);
  fs.unlinkSync(path.join(input, 'acecode-windows-arm64/acecode-computer-use.exe'));
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /windows-arm64.*acecode-computer-use\.exe/);
});

test('empty Windows Computer Use runtime rejects npm packaging', (t) => {
  const { input, run } = fixture(t);
  write(input, 'acecode-windows-x64/acecode-computer-use.exe', '');
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /windows-x64.*Computer Use runtime/);
});

test('missing macOS Computer Use runtime rejects npm packaging', (t) => {
  const { input, run } = fixture(t);
  fs.unlinkSync(path.join(input, 'acecode-macos-arm64/acecode-computer-use'));
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /macos-arm64.*acecode-computer-use/);
});

test('missing macOS iOS Simulator runtime rejects npm packaging', (t) => {
  const { input, run } = fixture(t);
  fs.unlinkSync(path.join(input, 'acecode-macos-x64/ACECode.app/Contents/MacOS/acecode-ios-sim'));
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /macos-x64.*iOS Simulator runtime/);
});

test('macOS app bundle must also contain its bridge', (t) => {
  const { input, run } = fixture(t);
  fs.unlinkSync(path.join(input, 'acecode-macos-arm64/ACECode.app/Contents/Resources/channels/whatsapp/bridge.mjs'));
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /macos-arm64 app bundle.*missing WhatsApp bridge asset: bridge.mjs/);
});

test('macOS app bundle must also contain its Computer Use helper', (t) => {
  const { input, run } = fixture(t);
  fs.unlinkSync(path.join(input, 'acecode-macos-x64/ACECode.app/Contents/MacOS/acecode-computer-use'));
  const result = run();
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /macos-x64.*Computer Use runtime/);
});
