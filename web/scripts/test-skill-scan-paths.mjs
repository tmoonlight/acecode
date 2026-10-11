import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

// Real settings components; API persistence/error responses are isolated fixtures.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-skill-paths-'));
const fixture = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {SettingsPage} from '/src/components/SettingsPage.jsx';
import {api} from '/src/lib/api.js';
import {i18n} from '/src/i18n/index.js';
import '/src/styles/globals.css';
const defaults=['.acecode/skills','.agent/skills','.opencode/skills','.opencode/skill','.agents/skills','.claude/skills','~/.acecode/skills','~/.agent/skills','~/.config/opencode/skills','~/.config/opencode/skill','~/.opencode/skills','~/.opencode/skill','~/.agents/skills','~/.claude/skills','~/.cache/opencode/skills'];
let config={paths:defaults,default_paths:defaults,customized:false};
window.calls=[];window.i18n=i18n;
Object.assign(api,{
 listSkills:async()=>[],listWorkspaces:async()=>[],
 getUpgradeConfig:async()=>({base_url:'https://example.invalid'}),
 getToolchainConfig:async()=>({toolchains:[]}),
 getTerminalConfig:async()=>({}),getDataDirectory:async()=>({effective_dir:'C:/fixture'}),
 getSkillScanPaths:async()=>config,
 setSkillScanPaths:async paths=>{window.calls.push(paths);if(window.failNext){window.failNext=false;throw new Error('fixture failure');}config={...config,paths:paths??defaults,customized:paths!==null};return config;}
});
createRoot(document.getElementById('root')).render(<SettingsPage initialNavKey="skills" onClose={()=>{}}/>);
`;
const virtual = path.join(web, '__skill-paths-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web, logLevel: 'error', server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{ name: 'skill-paths-fixture', enforce: 'pre',
    resolveId(id) { return id === '/__skill-paths-fixture.jsx' ? virtual : undefined; },
    load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
    configureServer(vite) { vite.middlewares.use(async (req, res, next) => {
      if (req.url !== '/skill-paths-fixture') return next();
      res.setHeader('Content-Type', 'text/html');
      res.end(await vite.transformIndexHtml('/skill-paths-fixture', '<!doctype html><html lang="zh-CN" data-theme="light"><meta name="viewport" content="width=device-width, initial-scale=1"><body><div id="root"></div><script type="module" src="/__skill-paths-fixture.jsx"></script></body></html>'));
    }); },
  }],
});
let browser;
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  page.setDefaultTimeout(20000);
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => localStorage.setItem('ace.locale', 'zh-CN'));
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/skill-paths-fixture`);
  const link = page.getByRole('link', { name: '设置 Skill 扫描路径' });
  await link.focus();
  await page.keyboard.press('Enter');
  const editor = page.locator('#skill-scan-paths');
  await page.waitForFunction(() => document.activeElement?.id === 'skill-scan-paths');
  assert.ok((await editor.inputValue()).includes('.agents/skills'));
  assert.ok(await page.getByRole('button', { name: '保存路径', exact: true }).isDisabled());
  await page.screenshot({ path: path.join(output, 'config-light.png') });
  await editor.fill('.workspace-skills\n\n  C:/共享技能  \n~/.codex/skills');
  await page.evaluate(() => { window.failNext = true; });
  await page.getByRole('button', { name: '保存路径', exact: true }).click();
  await page.getByText('保存 Skill 扫描路径失败：fixture failure', { exact: true }).waitFor();
  assert.ok((await editor.inputValue()).includes('C:/共享技能'));
  await page.getByRole('button', { name: '保存路径', exact: true }).click();
  await page.getByText('已保存，技能扫描路径已更新', { exact: true }).waitFor();
  assert.deepEqual(await page.evaluate(() => window.calls.at(-1)), ['.workspace-skills', 'C:/共享技能', '~/.codex/skills']);
  await page.getByRole('button', { name: '技能', exact: true }).click();
  await link.click();
  await page.waitForFunction(() => document.activeElement?.id === 'skill-scan-paths');
  assert.equal(await editor.inputValue(), '.workspace-skills\nC:/共享技能\n~/.codex/skills');
  await editor.fill('');
  await page.getByRole('button', { name: '保存路径', exact: true }).click();
  await page.getByText('已保存，技能扫描路径已更新', { exact: true }).waitFor();
  assert.deepEqual(await page.evaluate(() => window.calls.at(-1)), []);
  await page.getByRole('button', { name: '恢复默认', exact: true }).click();
  await page.waitForFunction(() => document.querySelector('#skill-scan-paths')?.value.includes('.agents/skills'));
  assert.equal(await page.evaluate(() => window.calls.at(-1)), null);
  assert.ok(await page.getByRole('button', { name: '恢复默认', exact: true }).isDisabled());
  for (const width of [1280, 480]) {
    await page.setViewportSize({ width, height: 900 });
    await page.evaluate(() => { document.documentElement.dataset.theme = 'dark'; });
    await editor.scrollIntoViewIfNeeded();
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    const box = await editor.boundingBox();
    assert.ok(box.x >= 0 && box.x + box.width <= width);
    await page.screenshot({ path: path.join(output, `config-dark-${width}.png`) });
  }
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.evaluate(() => window.i18n.changeLanguage('en-US'));
  await page.getByRole('button', { name: 'Skills', exact: true }).click();
  await page.getByRole('link', { name: 'Configure Skill scan paths' }).click();
  await page.getByRole('textbox', { name: 'Skill scan paths', exact: true }).waitFor();
  await page.screenshot({ path: path.join(output, 'config-english.png') });
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ ok: true, coverage: 'keyboard link/focus, loaded defaults, failure retains draft, retry, persistence across navigation, empty/reset, light/dark/narrow, English', output }));
} finally { await browser?.close(); await server.close(); }
