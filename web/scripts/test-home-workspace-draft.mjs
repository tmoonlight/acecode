import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
// Restore a draft while React still exposes the last acknowledged value. This
// deterministically exercises the local-echo filter used during fast switches.
const fixture = `
import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import { InputBar } from '/src/components/InputBar.jsx';
import '/src/styles/globals.css';
function Fixture() {
  const [draft, setDraft] = useState({ text: 'Saved target draft', revision: 0 });
  window.restoreDraft = text => setDraft(previous => ({ text, revision: previous.revision + 1 }));
  return <InputBar value={draft.text} composerContent={null} draftRevision={draft.revision}
    onChange={text => { window.lastLocalInput = text; }} />;
}
createRoot(document.getElementById('root')).render(<Fixture />);
`;
const virtual = path.join(web, '__home-draft-sync.jsx').replaceAll('\\', '/');
const html = `<html><body><div id="root"></div><script type="module">
import RefreshRuntime from '/@react-refresh'; RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{}; window.$RefreshSig$=()=>type=>type;
window.__vite_plugin_react_preamble_installed__=true;</script>
<script type="module" src="/__home-draft-sync.jsx"></script></body></html>`;
const baseline = process.argv.includes('--baseline');
const baselineSources = new Map(baseline ? [
  'src/components/InputBar.jsx', 'src/components/ChatView.jsx', 'src/App.jsx',
].map(relative => [path.join(web, relative).replaceAll('\\', '/'),
  execFileSync('git', ['show', '509a8008:web/' + relative], { cwd: web, encoding: 'utf8' })]) : []);
const server = await createServer({ root: web, logLevel: 'error',
  plugins: [{ name: 'home-draft-sync-fixture', enforce: 'pre',
    resolveId(id) { if (id === '/__home-draft-sync.jsx') return virtual; },
    load(id) {
      const file = id.replaceAll('\\', '/');
      if (file === virtual) return fixture;
      return baselineSources.get(file);
    },
    configureServer(vite) { vite.middlewares.use((req, res, next) => {
      if (req.url?.split('?')[0] !== '/__home-draft-sync') return next();
      res.setHeader('Content-Type', 'text/html'); res.end(html);
    }); },
  }], server: { host: '127.0.0.1', port: 0, open: false, hmr: false } });
const workspaces = [
  { hash: 'workspace-a', name: 'Project A', cwd: '/fixture/a', active: true },
  { hash: 'workspace-b', name: 'Project B', cwd: '/fixture/b' },
];
const draftPath = hash => `/api/workspaces/${hash || '__no_workspace__'}/draft`;
const drafts = new Map([
  [draftPath(''), { text: 'Personal draft' }],
  [draftPath('workspace-a'), { text: 'Project A draft' }],
  [draftPath('workspace-b'), { text: 'Project B draft' }],
]);
const heldLoads = [];
const createRequests = [];
const clearedDrafts = [];
const draftWrites = [];
let failCreation = true;
let holdLoads = false;
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true,
    executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 }, reducedMotion: 'reduce' });
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  await page.routeWebSocket('**/ws/**', () => {});
  await page.route('**/api/**', async route => {
    const request = route.request(), url = new URL(request.url());
    let json = {};
    if (request.method() === 'POST' && url.pathname.endsWith('/sessions')) {
      createRequests.push(url.pathname);
      await route.fulfill(failCreation
        ? { status: 503, json: { error: 'fixture create failure' } }
        : { json: { session_id: 'created-session', workspace_hash: 'workspace-b', cwd: '/fixture/b' } });
      return;
    }
    if (request.method() === 'DELETE' && url.pathname.endsWith('/draft')) {
      const source = url.pathname;
      clearedDrafts.push(source);
      drafts.set(source, { text: '' });
      await route.fulfill({ json: { cleared: true, text: '' } });
      return;
    }
    if (url.pathname.endsWith('/draft')) {
      if (request.method() === 'PUT') {
        const draft = request.postDataJSON();
        draftWrites.push({ path: url.pathname, draft });
        drafts.set(url.pathname, draft);
      }
      json = drafts.get(url.pathname) || { text: '' };
      if (holdLoads && request.method() === 'GET') {
        await new Promise(resolve => heldLoads.push(resolve));
      }
    } else if (url.pathname === '/api/health') {
      json = { ok: true, version: '1.0.17', cwd: '/fixture/a', workspace_hash: 'workspace-a' };
    } else if (url.pathname === '/api/workspaces') json = workspaces;
    else if (url.pathname === '/api/config') json = { models: [], web: {} };
    else if (/sidebar|sessions/.test(url.pathname)) json = { sessions: [], total: 0, has_more: false };
    else if (/models|providers|skills|experts|commands|history/.test(url.pathname)) json = [];
    else if (/onboarding/.test(url.pathname)) json = { dismissed: true };
    await route.fulfill({ json });
  });
  const editor = page.locator('[data-ace-rich-composer]');
  const expectText = async text => {
    await page.waitForFunction(expected => {
      const input = document.querySelector('[data-ace-rich-composer]')?.cloneNode(true);
      input?.querySelectorAll('[data-slate-placeholder]').forEach(node => node.remove());
      return input?.textContent.replaceAll('\uFEFF', '').trim() === expected;
    }, text, { timeout: 10000 }).catch(async error => { console.error('expected', text, 'actual', await editor.textContent()); throw error; });
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  };
  const chooseWorkspace = async name => {
    await page.locator('[data-tour-target="home-workspace"]').click();
    await page.getByRole('button', { name: name === '无工作区' ? name : new RegExp(name), exact: name === '无工作区' }).last().click();
    await page.waitForFunction(expected => document.querySelector('.ace-home-project-select')?.textContent.trim() === expected, name);
  };
  const openWorkspaceDraft = index => page.locator('[data-sidebar-workspace-new-task]').nth(index).click();
  const origin = `http://127.0.0.1:${server.httpServer.address().port}`;
  await page.goto(`${origin}/__home-draft-sync`);
  await expectText('Saved target draft');
  await editor.fill('');
  await expectText('');
  await page.evaluate(() => window.restoreDraft('Saved target draft'));
  await expectText('Saved target draft');
  console.log('[pass] restoring a nonempty target draft cannot leave the editor blank');

  await editor.fill('Local edit before switching');
  await expectText('Local edit before switching');
  await page.evaluate(() => window.restoreDraft(''));
  await expectText('');
  await editor.fill('Next workspace edit');
  await expectText('Next workspace edit');
  await page.evaluate(() => window.restoreDraft('Local edit before switching'));
  await expectText('Local edit before switching');
  console.log('[pass] explicit draft restoration replaces stale local echoes, including empty drafts');

  await page.goto(`${origin}/`);
  await expectText('Personal draft');
  holdLoads = true;
  const coldLoad = page.waitForRequest(request => request.method() === 'GET'
    && new URL(request.url()).pathname === draftPath('workspace-a'));
  await chooseWorkspace('Project A');
  await coldLoad;
  await expectText('');
  assert.equal(drafts.get(draftPath('workspace-a')).text, 'Project A draft');
  assert.ok(heldLoads.length > 0, 'the uncached target draft request must be held');
  holdLoads = false;
  for (const release of heldLoads.splice(0)) release();
  await expectText('Project A draft');
  assert.equal(draftWrites.some(({ path, draft }) => path === draftPath('workspace-a') && !draft.text), false,
    'the temporary blank editor must never overwrite the saved target draft');
  console.log('[pass] a cold workspace load restores its saved text after the temporary blank state');
  await editor.fill('Project A edited');
  await chooseWorkspace('Project B');
  await expectText('Project B draft');
  await editor.fill('Project B edited');
  await chooseWorkspace('无工作区');
  await expectText('Personal draft');
  await editor.fill('Personal edited');
  for (let i = 0; i < 3; i += 1) {
    await chooseWorkspace('Project A');
    await expectText('Project A edited');
    await chooseWorkspace('Project B');
    await expectText('Project B edited');
    await chooseWorkspace('无工作区');
    await expectText('Personal edited');
  }
  await chooseWorkspace('Project B');
  await editor.fill('');
  await chooseWorkspace('Project A');
  await expectText('Project A edited');
  await chooseWorkspace('Project B');
  await expectText('');
  await editor.fill('Personal edited');
  await editor.press('End');
  await editor.pressSequentially(' extra');
  await chooseWorkspace('无工作区');
  await expectText('Personal edited');
  await chooseWorkspace('Project B');
  await expectText('Personal edited extra');
  await editor.fill('Project B edited');
  await chooseWorkspace('无工作区');
  await expectText('Personal edited');
  console.log('[pass] dropdown restores independent drafts across workspaces and no workspace');

  await openWorkspaceDraft(0);
  await expectText('Project A edited');
  await openWorkspaceDraft(1);
  await expectText('Project B edited');
  await page.getByRole('button', { name: '新建任务', exact: true }).click();
  await expectText('Personal edited');
  console.log('[pass] sidebar and dropdown restore the same workspace drafts');

  holdLoads = true;
  await chooseWorkspace('Project A');
  await expectText('Project A edited');
  await chooseWorkspace('Project B');
  await expectText('Project B edited');
  await editor.fill('Project B newest');
  assert.ok(heldLoads.length > 0, 'the delayed draft request must actually run');
  holdLoads = false;
  for (const release of heldLoads.splice(0).reverse()) release();
  await page.waitForTimeout(400);
  await expectText('Project B newest');
  await chooseWorkspace('Project A');
  await expectText('Project A edited');
  await chooseWorkspace('无工作区');
  await expectText('Personal edited');
  console.log('[pass] delayed draft responses cannot cross workspaces or overwrite newer edits');

  await chooseWorkspace('Project B');
  await expectText('Project B newest');
  const creation = page.waitForResponse(response => response.request().method() === 'POST'
    && new URL(response.url()).pathname.endsWith('/sessions'));
  await editor.press('Enter');
  await creation;
  await page.getByText(/新建会话失败:.*fixture create failure/).waitFor();
  await expectText('Project B newest');
  assert.deepEqual(createRequests, ['/api/workspaces/workspace-b/sessions']);
  console.log('[pass] failed submission preserves the selected workspace draft');

  failCreation = false;
  const cleared = page.waitForResponse(response => response.request().method() === 'DELETE'
    && new URL(response.url()).pathname.endsWith('/draft'));
  await editor.press('Enter');
  await cleared;
  assert.deepEqual(clearedDrafts, [draftPath('workspace-b')]);
  assert.equal(drafts.get(draftPath('')).text, 'Personal edited');
  assert.equal(drafts.get(draftPath('workspace-a')).text, 'Project A edited');
  assert.deepEqual(errors, []);
  console.log('[pass] successful submission clears only the selected workspace draft');
  await page.goto('about:blank');
} finally {
  for (const release of heldLoads.splice(0)) release();
  await browser?.close();
  await server.close();
}
