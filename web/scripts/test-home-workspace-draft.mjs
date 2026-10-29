import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const server = await createServer({ root: web, logLevel: 'error',
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false } });
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
      if (request.method() === 'PUT') drafts.set(url.pathname, request.postDataJSON());
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
      const input = document.querySelector('[data-ace-rich-composer]');
      return input?.textContent.replaceAll('\uFEFF', '').trim() === expected;
    }, text, { timeout: 10000 });
  };
  const chooseWorkspace = async name => {
    await page.locator('[data-tour-target="home-workspace"]').click();
    await page.getByRole('button', { name: name === '无工作区' ? name : new RegExp(name), exact: name === '无工作区' }).last().click();
    await page.waitForFunction(expected => document.querySelector('.ace-home-project-select')?.textContent.trim() === expected, name);
  };
  const openWorkspaceDraft = index => page.locator('[data-sidebar-workspace-new-task]').nth(index).click();
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/`);
  await expectText('Personal draft');
  await chooseWorkspace('Project A');
  await expectText('Personal draft');
  await editor.fill('Personal draft edited');
  await chooseWorkspace('Project B');
  await expectText('Personal draft edited');
  await chooseWorkspace('无工作区');
  await expectText('Personal draft edited');
  console.log('[pass] home dropdown preserves the active draft across workspace and no-workspace targets');

  await openWorkspaceDraft(0);
  await expectText('Project A draft');
  holdLoads = true;
  await chooseWorkspace('Project B');
  await expectText('Project A draft');
  await editor.fill('Project A newest edit');
  assert.ok(heldLoads.length > 0, 'the delayed draft request must actually run');
  holdLoads = false;
  for (const release of heldLoads.splice(0)) release();
  await page.waitForTimeout(400);
  await expectText('Project A newest edit');
  await chooseWorkspace('无工作区');
  await expectText('Project A newest edit');
  console.log('[pass] late draft hydration cannot overwrite edits after changing the target workspace');

  await chooseWorkspace('Project B');
  await openWorkspaceDraft(1);
  await expectText('Project B draft');
  await openWorkspaceDraft(0);
  await expectText('Project A newest edit');
  await page.getByRole('button', { name: '新建任务', exact: true }).click();
  await expectText('Personal draft edited');
  assert.equal(drafts.get(draftPath('workspace-b')).text, 'Project B draft');
  assert.equal(drafts.get(draftPath('workspace-a')).text, 'Project A newest edit');
  assert.equal(drafts.get(draftPath('')).text, 'Personal draft edited');
  console.log('[pass] sidebar navigation restores each independent draft without overwriting destination drafts');

  await chooseWorkspace('Project B');
  const creation = page.waitForResponse(response => response.request().method() === 'POST'
    && new URL(response.url()).pathname.endsWith('/sessions'));
  await editor.press('Enter');
  await creation;
  await page.getByText(/新建会话失败:.*fixture create failure/).waitFor();
  await expectText('Personal draft edited');
  assert.deepEqual(createRequests, ['/api/workspaces/workspace-b/sessions']);
  assert.equal(drafts.get(draftPath('workspace-b')).text, 'Project B draft');
  console.log('[pass] submission uses the selected workspace and a failed request preserves the carried draft');

  failCreation = false;
  const cleared = page.waitForResponse(response => response.request().method() === 'DELETE'
    && new URL(response.url()).pathname.endsWith('/draft'));
  await editor.press('Enter');
  await cleared;
  assert.deepEqual(clearedDrafts, [draftPath('')]);
  assert.equal(drafts.get(draftPath('workspace-b')).text, 'Project B draft');
  assert.equal(drafts.get(draftPath('workspace-a')).text, 'Project A newest edit');
  assert.deepEqual(errors, []);
  console.log('[pass] successful submission clears the source draft and preserves other workspace drafts');
  await page.goto('about:blank');
} finally {
  for (const release of heldLoads.splice(0)) release();
  await browser?.close();
  await server.close();
}
