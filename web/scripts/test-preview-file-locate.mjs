import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

// Exercise the production App and its real menu/shortcut/list owners. The API
// fixtures never connect to a user daemon or reuse a browser profile.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-preview-file-locate-'));
const server = await createServer({ root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false } });
const session = { id: 'locate-session', session_id: 'locate-session', title: 'Locate fixture',
  cwd: 'C:/fixture', workspace_hash: 'fixture', active: true, updated_at: '2026-10-22T10:00:00Z' };
const messages = [
  { id: 'user-1', role: 'user', content: '定位测试' },
  { id: 'assistant-1', role: 'assistant', content: '[target.js](src/deep/target.js)\n\n[other.js](other.js)\n\n[outside.js](D:/outside/outside.js)' },
];
const errors = [], directories = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1600, height: 900 } });
  page.setDefaultTimeout(15000);
  page.on('pageerror', e => errors.push(e.message));
  await page.addInitScript(() => {
    class FixtureWebSocket extends EventTarget {
      static OPEN = 1; static CLOSED = 3;
      constructor() { super(); this.readyState = 1; setTimeout(() => { this.onopen?.({}); this.dispatchEvent(new Event('open')); }, 0); }
      send() {} close() { this.readyState = 3; }
    }
    window.WebSocket = FixtureWebSocket;
    if (location.search.includes('desktop')) window.__ACECODE_DESKTOP_SHELL__ = true;
    localStorage.setItem('acecode.uiPrefs.v1', JSON.stringify({ view: 'single', sidebarCollapsed: false,
      sidePanelCollapsed: false, sidePanelListCollapsed: false, fontSize: 'medium' }));
    window.locateAnimations = [];
    document.addEventListener('animationstart', e => {
      if (e.animationName.startsWith('ace-file-locate')) window.locateAnimations.push({ name: e.animationName,
        path: e.target.closest('[data-desktop-file-path]')?.getAttribute('data-desktop-file-path') });
    });
  });
  await page.route('**/api/**', async route => {
    const { pathname, searchParams } = new URL(route.request().url());
    let data = {};
    if (pathname === '/api/health') data = { status: 'ok', version: '1.0.0', cwd: session.cwd };
    else if (pathname.includes('onboarding')) data = { dismissed: true };
    else if (pathname === '/api/config/ui-preferences') data = { theme: 'light', color_theme: 'blue', font_size: 'medium' };
    else if (pathname === '/api/workspaces') data = [{ hash: 'fixture', cwd: session.cwd, name: 'Locate project' }];
    else if (pathname.endsWith('/messages')) data = { messages, events: [], busy: false, title: session.title, cwd: session.cwd };
    else if (pathname.endsWith('/sessions') || pathname.endsWith('/session-sidebar')) data = pathname === '/api/session-sidebar' ? [] : [session];
    else if (pathname.endsWith('/resume') || /^\/api\/sessions\/[^/]+$/.test(pathname)) data = session;
    else if (pathname.endsWith('/model')) data = { name: 'fixture', provider: 'openai', model: 'test' };
    else if (pathname.includes('models')) data = { models: [], profiles: [] };
    else if (pathname.includes('history')) data = { items: [], history: [] };
    else if (pathname.includes('commands')) data = { commands: [] };
    else if (pathname.includes('skills')) data = { skills: [] };
    else if (pathname.includes('experts')) data = { experts: [] };
    else if (pathname.includes('subagents')) data = { tasks: [] };
    else if (pathname.includes('loops') || pathname.includes('/pty') || pathname.includes('/themes')) data = [];
    else if (pathname === '/api/files') {
      const dir = searchParams.get('path') || ''; directories.push(dir);
      const entry = (name, kind = 'file') => ({ name, path: dir ? `${dir}/${name}` : name, kind });
      if (!dir) data = [entry('src', 'dir'), entry('other.js')];
      else if (dir === 'src') data = [entry('deep', 'dir')];
      else if (dir === 'src/deep') {
        await new Promise(resolve => setTimeout(resolve, 150));
        data = [...Array.from({ length: 70 }, (_, i) => entry(`a${String(i).padStart(2, '0')}.js`)), entry('target.js')];
      } else throw new Error(`Unexpected directory request: ${dir}`);
    } else if (pathname.includes('/files/content')) {
      await route.fulfill({ status: 200, contentType: 'text/plain', body: 'export const answer = 42;\n' }); return;
    } else if (pathname.includes('/files/edit')) data = { text: 'export const answer = 42;\n', read_id: 'fixture-read' };
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(data) });
  });
  const count = () => page.evaluate(() => window.locateAnimations.filter(a => a.name === 'ace-file-locate-highlight').length);
  const waitPulse = async before => {
    try {
      await page.waitForFunction(n => window.locateAnimations.filter(a => a.name === 'ace-file-locate-highlight').length > n, before);
    } catch (error) {
      console.error('Locate state', await page.evaluate(async () => {
        const { sessionWorkbench } = await import('/src/lib/sessionWorkbench.js');
        return { request: sessionWorkbench.get('session:locate-session', 'fileLocate'),
          animations: window.locateAnimations, dialogs: [...document.querySelectorAll('[role="dialog"], [role="menu"]')].map(e => e.textContent),
          selected: document.querySelector('[data-desktop-file-selected="true"]')?.outerHTML,
          tree: document.querySelector('.ace-file-tree')?.getBoundingClientRect().toJSON() };
      }), errors);
      throw error;
    }
  };
  const row = page.locator('.ace-file-tree [data-desktop-file-path="src/deep/target.js"]');
  const activePath = () => page.locator('.ace-preview-details-tab[aria-selected="true"]').getAttribute('data-desktop-preview-tab-path');
  const assertVisibleSelected = async () => {
    assert.equal(await row.getAttribute('data-desktop-file-selected'), 'true');
    assert.equal(await row.evaluate(e => {
      const r = e.getBoundingClientRect(), p = e.closest('.ace-file-tree').getBoundingClientRect();
      return r.top >= p.top && r.bottom <= p.bottom && p.width > 0;
    }), true);
  };
  const menuLocate = async target => {
    await target.click({ button: 'right' });
    await page.getByRole('menuitem', { name: /在文件列表展示/ }).click();
  };
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/${process.argv.includes('--desktop') ? '?desktop' : ''}`, { waitUntil: 'commit' });
  await page.locator('.ace-sidebar-session-title-button').filter({ hasText: session.title }).first().click();
  const transcript = page.locator('[data-conversation-find-root]');
  await transcript.getByText('target.js', { exact: true }).click();
  await page.locator('[data-desktop-preview-path="src/deep/target.js"]').waitFor();
  if (process.argv.includes('--desktop')) {
    const editor = page.locator('[data-ace-editable-preview-text]');
    await editor.fill('export const draft = 99;\n');
    await page.keyboard.press('Control+BracketLeft');
    await waitPulse(0);
    assert.equal(await editor.inputValue(), 'export const draft = 99;\n');
    await editor.fill('export const answer = 42;\n');
  }
  // Start from a different list tab and a collapsed list, with no loaded parents.
  await page.locator('.ace-side-panel').getByRole('tab', { name: '变更', exact: true }).click();
  await page.locator('.ace-side-panel').getByRole('button', { name: '收起列表面板', exact: true }).click();
  const initialPulseCount = await count();
  await page.keyboard.press('Control+BracketLeft');
  await waitPulse(initialPulseCount);
  await assertVisibleSelected();
  assert.equal(await activePath(), 'src/deep/target.js');
  assert.ok(directories.includes('src') && directories.includes('src/deep'));
  assert.equal(directories.includes('src/deep/target.js'), false);
  const motion = await row.locator('.ace-file-leaf-content').evaluate(e => {
    const animation = e.getAnimations()[0]; animation.pause(); animation.currentTime = 210;
    return getComputedStyle(e).transform;
  });
  assert.notEqual(motion, 'none'); assert.notEqual(motion, 'matrix(1, 0, 0, 1, 0, 0)');
  await page.screenshot({ path: path.join(output, 'locate-highlight.png') });
  let before = await count();
  await page.keyboard.press('Control+BracketLeft'); await waitPulse(before); await assertVisibleSelected();
  console.log('[pass] shortcut opens list, lazily expands parents, scrolls/selects and restarts visible scale highlight');

  // Right-click a background tab without activating it.
  await transcript.getByText('other.js', { exact: true }).click();
  const targetTab = page.locator('.ace-preview-details-tab[data-desktop-preview-tab-path="src/deep/target.js"]');
  before = await count(); await menuLocate(targetTab); await waitPulse(before);
  await assertVisibleSelected(); assert.equal(await activePath(), 'other.js');
  console.log('[pass] background tab context menu locates its own file and preserves the active preview');

  // Content menu, repeated requests, and reduced-motion feedback.
  await targetTab.click();
  before = await count();
  await menuLocate(page.locator('[data-desktop-preview-path="src/deep/target.js"]').first());
  await waitPulse(before); await assertVisibleSelected();
  await page.emulateMedia({ reducedMotion: 'reduce' });
  before = await count(); await page.keyboard.press('Control+BracketLeft'); await waitPulse(before);
  assert.equal(await row.locator('.ace-file-leaf-content').evaluate(e => getComputedStyle(e).animationName), 'none');
  await page.waitForFunction(() => !document.querySelector('.ace-file-locate-highlight'));
  assert.equal(await row.getAttribute('data-desktop-file-selected'), 'true');
  // Ordinary selection changes must not replay a consumed explicit reveal.
  before = await count();
  await page.locator('.ace-file-tree [data-desktop-file-path="other.js"]').click();
  await row.click();
  await page.waitForTimeout(100);
  assert.equal(await count(), before);
  console.log('[pass] content menu and reduced motion retain feedback and final selection');

  // External files must not change the collapsed list, selection, or scroll.
  await transcript.getByText('outside.js', { exact: true }).click();
  await page.locator('.ace-side-panel').getByRole('button', { name: '收起列表面板', exact: true }).click();
  const state = () => page.evaluate(() => ({ collapsed: document.querySelector('.ace-side-panel-shell')?.dataset.collapsed,
    selected: document.querySelector('.ace-file-tree [data-desktop-file-selected="true"]')?.dataset.desktopFilePath,
    scroll: document.querySelector('.ace-file-tree')?.scrollTop, animations: window.locateAnimations.length }));
  const prior = await state();
  await page.keyboard.press('Control+BracketLeft');
  await menuLocate(page.locator('.ace-preview-details-tab[aria-selected="true"]'));
  await page.waitForTimeout(150);
  assert.deepEqual(await state(), prior);
  assert.equal(await page.getByText('操作不可用', { exact: true }).count(), 0);
  console.log('[pass] outside-workspace shortcut and menu have no side effects');

  // Narrow layout uses the existing right popup and can reveal its list.
  await page.setViewportSize({ width: 600, height: 900 });
  await page.waitForTimeout(350);
  const rightToggle = page.locator('[data-sidebar-toggle="right"]');
  if (await rightToggle.getAttribute('aria-expanded') !== 'true') await rightToggle.click();
  await targetTab.click();
  before = await count(); await page.keyboard.press('Control+BracketLeft'); await waitPulse(before);
  await assertVisibleSelected();
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  await page.screenshot({ path: path.join(output, 'locate-narrow.png') });
  console.log('[pass] narrow layout reveals the selected row without page overflow');
  assert.deepEqual(errors, []);
  console.log(`Screenshots: ${output}`);
} finally {
  await browser?.close();
  await server.close();
}
