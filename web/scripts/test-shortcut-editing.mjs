import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

// Real App/Settings/ChatView; all daemon requests and WebSockets are fixtures.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-shortcut-editing-'));
const server = await createServer({ root: web, configFile: path.join(web, 'vite.config.js'),
  logLevel: 'error', server: { host: '127.0.0.1', port: 0 } });
const channels = ['qq', 'weixin', 'feishu', 'dingtalk', 'telegram', 'discord', 'line'].map(platform => ({
  platform, configured: false, enabled: false, state: 'disabled', contacts: [], pending: [], bindings: [],
}));
const sessions = [];
const calls = [];
const errors = [];
const checks = [];
const pass = (name) => { checks.push(name); console.log('[pass]', name); };
const reasoning = { supported: true, default_enabled: true, supported_efforts: ['low', 'high', 'max'], default_effort: 'high' };
const model = (effort = null) => ({ name: 'fixture', provider: 'openai', model: 'fixture-model',
  reasoning: { ...reasoning, effort: effort || 'high' }, reasoning_effort: effort });
const efforts = {};
const drafts = {};
let browser;
let page;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const browserContext = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  page = await browserContext.newPage();
  page.setDefaultTimeout(15000);
  page.on('pageerror', error => errors.push(error.message));
  await page.context().addInitScript(() => {
    window.keyRecords = [];
    window.addEventListener('keydown', event => {
      if (!event.ctrlKey && !event.metaKey) return;
      const row = { code: event.code, key: event.key, alt: event.altKey, shift: event.shiftKey, composing: event.isComposing,
        altGraph: event.getModifierState('AltGraph'), target: event.target.tagName,
        overlays: [...document.querySelectorAll('[data-ace-native-overlay="blocking"]')].filter(el => el.getClientRects().length).map(el => el.outerHTML.slice(0, 100)) };
      window.keyRecords.push(row);
      setTimeout(() => { row.prevented = event.defaultPrevented; }, 0);
    }, true);
    localStorage.setItem('ace.locale', 'zh-CN');
    class FixtureWebSocket extends EventTarget {
      static OPEN = 1; static CLOSED = 3;
      constructor() { super(); this.readyState = 1; setTimeout(() => this.dispatchEvent(new Event('open')), 0); }
      send() {} close() { this.readyState = 3; }
    }
    window.WebSocket = FixtureWebSocket;
    window.sentFrames = [];
    FixtureWebSocket.prototype.send = data => {
      try { window.sentFrames.push(JSON.parse(data)); }
      catch { window.sentFrames.push({ terminalData: data }); }
    };
  });
  await page.context().route('**/api/**', async route => {
    const req = route.request();
    const pathname = new URL(req.url()).pathname;
    const body = req.postData() ? req.postDataJSON() : null;
    calls.push({ path: pathname, method: req.method(), body });
    const sid = pathname.match(/\/sessions\/([^/]+)/)?.[1];
    let data = {};
    if (pathname === '/api/health') data = { status: 'ok', cwd: 'C:/shortcut-fixture', version: 'fixture', console: { available: true } };
    else if (pathname === '/api/channels') data = { platforms: channels };
    else if (/\/channels\/[^/]+\/enabled$/.test(pathname)) {
      const channel = channels.find(item => pathname.includes(`/${item.platform}/`));
      Object.assign(channel, { enabled: body.enabled, state: body.enabled ? 'connected' : 'disabled' });
      data = { platforms: channels };
    } else if (/\/channels\/.*\/requests\//.test(pathname)) {
      channels.find(item => pathname.includes(`/${item.platform}/`)).pending = [];
      data = { platforms: channels };
    } else if (pathname.endsWith('/sessions') && req.method() === 'POST') {
      const session = { id: `shortcut-${sessions.length + 1}`, title: `Shortcut task ${sessions.length + 1}`,
        created_at: '2026-10-07T08:00:00Z', cwd: 'C:/shortcut-fixture', model: 'fixture-model', provider: 'openai' };
      sessions.push(session); data = { ...session, session_id: session.id };
    } else if (pathname.endsWith('/sessions')) data = { sessions };
    else if (pathname.endsWith('/draft')) {
      if (req.method() === 'PUT') drafts[sid] = body;
      data = drafts[sid] || { text: '' };
    }
    else if (pathname.endsWith('/reasoning')) { efforts[sid] = body.effort; data = model(body.effort); }
    else if (pathname.endsWith('/model')) data = model(efforts[sid]);
    else if (pathname.includes('/messages')) data = { messages: [], events: [], busy: false, status: 'idle' };
    else if (pathname.includes('models')) data = { models: [model()], profiles: [model()], default_model: 'fixture' };
    else if (pathname.includes('workspaces')) data = { workspaces: [] };
    else if (pathname.includes('history')) data = { items: [], history: [] };
    else if (pathname.includes('commands')) data = { commands: [] };
    else if (pathname.includes('skills')) data = { skills: [] };
    else if (pathname.includes('experts')) data = { experts: [] };
    else if (pathname.includes('subagents')) data = { tasks: [] };
    else if (pathname.includes('onboarding')) data = { dismissed: true, completed: true };
    else if (pathname.endsWith('/api/pty') && req.method() === 'POST') data = { id: 'fixture-pty' };
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(data) });
  });
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}`, { waitUntil: 'networkidle' });
  const recorder = page.locator('[data-shortcut-recorder]');
  const settings = page.locator('[data-settings-window]');
  const open = async () => {
    await page.waitForFunction(() => ![...document.querySelectorAll('[data-ace-native-overlay="blocking"]')]
      .some(el => el.getClientRects().length && !el.matches('[data-settings-mask]')));
    await page.keyboard.press('Control+/');
    await page.locator('[data-shortcut-search]').waitFor();
  };
  const edit = async id => {
    await page.locator(`[data-edit-shortcut="${id}"]`).click();
    await recorder.waitFor();
    assert.equal(await recorder.evaluate(el => el === document.activeElement), true);
  };
  const save = async () => {
    await page.getByRole('button', { name: '保存', exact: true }).click();
    await recorder.waitFor({ state: 'detached' });
  };
  const assign = async (id, keys) => { await edit(id); await recorder.press(keys); await save(); };
  const emit = async (type, payload, session_id) => page.evaluate(async ({ type, payload, session_id }) => {
    const { connection } = await import('/src/lib/connection.js');
    connection.dispatchEvent(new CustomEvent('message', { detail: { type, payload, session_id } }));
  }, { type, payload, session_id });
  await open();
  assert.equal(await page.locator('[data-edit-shortcut]').count(), 21);
  await edit('newSession');
  await recorder.press('Control+Alt+j');
  assert.equal(await recorder.inputValue(), 'Ctrl+Alt+J');
  assert.equal(sessions.length, 0, 'recording never executes an action');
  await page.getByRole('button', { name: '取消', exact: true }).click();
  assert.match(await page.locator('[data-shortcut-id="newSession"]').innerText(), /Ctrl\s*Alt\s*N/);
  await edit('newSession');
  await recorder.press('Control+k');
  await page.getByText(/此快捷键已用于“全局搜索”/).waitFor();
  assert.equal(await page.getByRole('button', { name: '保存', exact: true }).isDisabled(), true);
  await recorder.press('Control+c');
  await page.getByText(/此组合键由系统/).waitFor();
  await recorder.press('Control+Alt+j');
  await save();
  await page.keyboard.press('Escape');
  await settings.waitFor({ state: 'detached' });
  await page.keyboard.press('Control+Alt+n');
  assert.equal(sessions.length, 0);
  await page.keyboard.press('Control+Alt+j');
  await page.waitForFunction(() => document.querySelector('.ace-topbar-session-title')?.textContent.includes('Shortcut task 1'));
  await page.waitForLoadState('networkidle');
  pass('every key has an edit icon; capture/cancel/conflict/reserved-key validation and live replacement');

  await page.reload({ waitUntil: 'networkidle' });
  await page.keyboard.press('Control+Alt+j');
  await page.waitForFunction(() => document.querySelector('.ace-topbar-session-title')?.textContent.includes('Shortcut task 2'));
  await page.waitForLoadState('networkidle');
  await open();
  await assign('search', 'Control+Alt+p');
  await assign('console', 'Control+Alt+t');
  await page.keyboard.press('Escape');
  await settings.waitFor({ state: 'detached' });
  await page.keyboard.press('Control+k');
  assert.equal(await page.locator('[data-search-palette]').count(), 0);
  await page.keyboard.press('Control+Alt+p');
  await page.locator('[data-search-palette]').waitFor();
  await page.keyboard.press('Control+Alt+p');
  await page.locator('[data-search-palette]').waitFor({ state: 'detached' });
  assert.equal(await page.locator('.ace-topbar button[title*="Ctrl+Alt+P"]').count(), 1);
  const consoleButton = page.locator('.ace-topbar button[title*="Ctrl+Alt+T"]');
  const beforeConsole = await consoleButton.getAttribute('aria-pressed');
  await page.keyboard.press('Control+Backquote');
  assert.equal(await consoleButton.getAttribute('aria-pressed'), beforeConsole);
  if (beforeConsole === 'false') await page.keyboard.press('Control+Alt+t');
  await page.getByRole('button', { name: '新建终端(默认 shell)', exact: true }).click();
  await page.locator('.xterm-helper-textarea').waitFor({ state: 'attached' });
  await page.locator('.xterm-helper-textarea').focus();
  await page.keyboard.press('Control+Alt+p');
  await page.locator('[data-search-palette]').waitFor();
  await page.keyboard.press('Control+Alt+p');
  await page.locator('[data-search-palette]').waitFor({ state: 'detached' });
  await page.locator('.xterm-helper-textarea').focus();
  await page.keyboard.press('Control+Alt+t');
  assert.equal(await consoleButton.getAttribute('aria-pressed'), 'false');
  pass('reload persistence, updated hints and remapped terminal forwarding');

  await open();
  const sibling = await page.context().newPage();
  await sibling.goto(page.url(), { waitUntil: 'networkidle' });
  await sibling.keyboard.press('Control+/');
  await sibling.locator('[data-edit-shortcut="newSession"]').waitFor();
  await assign('newSession', 'Control+Alt+u');
  await sibling.waitForFunction(() => document.querySelector('[data-shortcut-id="newSession"]')?.textContent.includes('U'));
  await sibling.close();
  await edit('focusInput');
  await recorder.press('Control+Alt+m');
  await page.evaluate(() => {
    window.restoreSetItem = Storage.prototype.setItem;
    Storage.prototype.setItem = function(key, value) {
      if (key === 'acecode.shortcuts.v1') throw new Error('fixture quota error');
      return window.restoreSetItem.call(this, key, value);
    };
  });
  await page.getByRole('button', { name: '保存', exact: true }).click();
  await page.getByText('无法保存快捷键，请检查本地存储后重试。').waitFor();
  await page.evaluate(() => { Storage.prototype.setItem = window.restoreSetItem; });
  await page.getByRole('button', { name: '取消', exact: true }).click();
  assert.match(await page.locator('[data-shortcut-id="focusInput"]').innerText(), /Ctrl\s*Alt\s*I/);
  pass('other windows update immediately and failed storage writes keep old bindings');

  await assign('send', 'Control+Alt+s');
  await assign('newline', 'Control+Alt+Enter');
  await assign('historyPrevious', 'Control+Alt+ArrowUp');
  await assign('historyNext', 'Control+Alt+ArrowDown');
  await assign('find', 'Control+Alt+f');
  await assign('save', 'Control+Alt+g');
  await page.keyboard.press('Escape');
  await settings.waitFor({ state: 'detached' });
  const composer = page.locator('[data-main-composer="true"] [contenteditable="true"]');
  await page.keyboard.press('Control+Alt+i');
  await composer.fill('Custom input');
  const inputCalls = () => calls.filter(call => call.method === 'POST' && call.path.endsWith('/messages'));
  const inputsBefore = inputCalls().length;
  await page.keyboard.press('Enter');
  assert.equal(inputCalls().length, inputsBefore);
  await page.keyboard.type('second line');
  await page.keyboard.press('Control+Alt+Enter');
  await page.keyboard.type('third line');
  assert.match(await composer.innerText(), /Custom input[\s\S]*second line[\s\S]*third line/);
  const submitted = page.waitForResponse(response => response.request().method() === 'POST' && response.url().endsWith('/messages'));
  await page.keyboard.press('Control+Alt+s');
  await submitted;
  assert.equal(inputCalls().length, inputsBefore + 1);
  await emit('busy_changed', { busy: false }, 'shortcut-2');
  await composer.fill('');
  await page.keyboard.press('ArrowUp');
  assert.ok(!(await composer.innerText()).includes('Custom input'));
  await page.keyboard.press('Control+Alt+ArrowUp');
  await page.waitForFunction(() => document.querySelector('[data-main-composer="true"] [contenteditable="true"]')?.textContent.includes('Custom input'));
  await page.keyboard.press('Control+Alt+ArrowDown');
  await page.keyboard.press('Control+f');
  assert.equal(await page.getByPlaceholder('搜索当前对话内容').count(), 0);
  await page.keyboard.press('Control+Alt+f');
  await page.getByPlaceholder('搜索当前对话内容').waitFor();
  await page.keyboard.press('Escape');
  assert.equal(await page.evaluate(async () => {
    const { isShortcut } = await import('/src/lib/shortcutBindings.js');
    return !isShortcut({ key: 's', code: 'KeyS', ctrlKey: true }, 'save')
      && isShortcut({ key: 'g', code: 'KeyG', ctrlKey: true, altKey: true }, 'save');
  }), true);
  pass('custom send, newline, both history keys, find and save matching');

  await open();
  await edit('dismiss');
  await recorder.press('Escape');
  assert.equal(await recorder.isVisible(), true, 'Escape is captured without dismissing the editor');
  await recorder.press('Control+Alt+q');
  await save();
  await page.keyboard.press('Escape');
  assert.equal(await settings.isVisible(), true);
  await page.keyboard.press('Control+Alt+q');
  await settings.waitFor({ state: 'detached' });
  await open();
  await edit('newSession');
  await page.getByRole('button', { name: '恢复默认', exact: true }).click();
  await recorder.waitFor({ state: 'detached' });
  assert.match(await page.locator('[data-shortcut-id="newSession"]').innerText(), /Ctrl\s*Alt\s*N/);
  assert.match(await page.locator('[data-shortcut-id="search"]').innerText(), /Ctrl\s*Alt\s*P/);
  pass('custom dismissal and individual default restoration');

  for (const [width, theme] of [[1440, 'light'], [420, 'dark']]) {
    await page.setViewportSize({ width, height: 900 });
    await page.evaluate(theme => document.documentElement.setAttribute('data-theme', theme), theme);
    await page.locator('.ace-settings-content').evaluate(el => { el.scrollTop = 0; });
    await page.screenshot({ path: path.join(output, `editable-shortcuts-${width}-${theme}.png`) });
    await edit('newSession');
    await recorder.press('Control+Alt+p');
    await page.getByText(/此快捷键已用于/).waitFor();
    await page.screenshot({ path: path.join(output, `shortcut-recorder-${width}-${theme}.png`) });
    await page.getByRole('button', { name: '取消', exact: true }).click();
  }
  assert.deepEqual(errors, []);
  await fs.writeFile(path.join(output, 'results.json'), JSON.stringify({ checks, errors }, null, 2));
  console.log('Evidence:', output);
} catch (error) {
  await page?.screenshot({ path: path.join(output, 'failure.png') });
  console.error('Runtime errors:', errors);
  console.error('Keys:', await page?.evaluate(() => window.keyRecords.slice(-6)));
  console.error('Frames:', await page?.evaluate(() => window.sentFrames.slice(-6)));
  console.error('Writes:', calls.filter(call => call.method !== 'GET').slice(-8));
  console.error('Page:', await page?.evaluate(() => document.body.innerText.slice(-1500)));
  console.error('Evidence:', output);
  throw error;
} finally {
  await browser?.close();
  await server.close();
}
