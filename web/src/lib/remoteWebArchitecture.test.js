import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('general settings keeps remote Web mode as its final card', () => {
  const page = source('components/SettingsPage.jsx');
  const backgroundIndex = page.indexOf('退出 ACECode 后继续运行后台进程');
  const remoteIndex = page.indexOf('data-remote-web-settings');
  const appearanceIndex = page.indexOf('// ─── 外观');

  assert.ok(backgroundIndex >= 0);
  assert.ok(remoteIndex > backgroundIndex);
  assert.ok(appearanceIndex > remoteIndex);
  assert.match(page, />远程 Web 模式</);
  assert.match(page, /127\.0\.0\.1 切换为 0\.0\.0\.0/);
  assert.match(page, />\s*复制连接\s*</);
});

run('remote Web card preserves token warning and network safety guidance', () => {
  const page = source('components/SettingsPage.jsx');
  assert.match(page, /此连接包含访问 Token，请勿将此连接公开给别人。/);
  assert.match(page, /系统防火墙、路由器或云安全组可能仍需放行端口/);
  assert.match(page, /可信 VPN 或 HTTPS 反向代理/);
  assert.match(page, /copyTextToSystemClipboard\(connection\.url\)/);
});

run('remote Web UI uses normalized state and listener rebind polling', () => {
  const page = source('components/SettingsPage.jsx');
  const api = source('lib/api.js');
  assert.match(page, /normalizeRemoteWebState/);
  assert.match(page, /waitForRemoteWebMode\(api, next/);
  assert.match(page, /selectRemoteWebConnection/);
  assert.match(page, /connection\.kind === 'computer_name'/);
  assert.match(page, /计算机名/);
  assert.match(page, /remoteWebBusyRef\.current = true/);
  assert.match(page, /remoteWebBusyRef\.current = false/);
  assert.match(api, /getRemoteWeb:[\s\S]*\/api\/config\/remote-web/);
  assert.match(api, /setRemoteWeb:[\s\S]*\{enabled: !!enabled\}/);
});

console.log('remoteWebArchitecture tests passed');
