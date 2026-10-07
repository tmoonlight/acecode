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
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-shortcuts-channels-'));
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
  page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  page.setDefaultTimeout(15000);
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => {
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
  await page.route('**/api/**', async route => {
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
  await page.locator('.ace-topbar').waitFor();
  await page.keyboard.press('Control+/');
  const search = page.locator('[data-shortcut-search]');
  await search.waitFor();
  assert.equal(await search.evaluate(el => el === document.activeElement), true);
  assert.equal(await page.locator('[data-shortcut-id]').count(), 19);
  await search.fill('Ctrl+Shift+-');
  assert.equal(await page.locator('[data-shortcut-id]').count(), 1);
  assert.equal(await page.locator('[data-shortcut-id]').getAttribute('data-shortcut-id'), 'back');
  await search.fill('思维');
  assert.equal(await page.locator('[data-shortcut-id]').count(), 2);
  await search.fill('missing-shortcut');
  await page.getByText('没有找到匹配的快捷键').waitFor();
  await search.fill('');
  await page.locator('.ace-settings-content').evaluate(el => { el.scrollTop = 500; });
  const sticky = await page.locator('[data-shortcut-filters]').boundingBox();
  const content = await page.locator('.ace-settings-content').boundingBox();
  assert.ok(Math.abs(sticky.y - content.y) <= 2, JSON.stringify({ sticky, content }));
  await page.waitForTimeout(250);
  await page.screenshot({ path: path.join(output, 'shortcuts-sticky.png') });
  pass('shortcut settings: 19 entries, name/key search, empty state, focus and sticky search');

  const beforeModal = sessions.length;
  await page.keyboard.press('Control+Alt+n');
  assert.equal(sessions.length, beforeModal);
  await page.keyboard.press('Escape');
  await search.waitFor({ state: 'detached' });
  await page.keyboard.press('Control+Alt+n');
  await page.waitForFunction(() => document.querySelector('.ace-topbar-session-title')?.textContent.includes('Shortcut task 1'));
  await page.locator('[data-main-composer="true"] [contenteditable="true"]').waitFor();
  await page.waitForLoadState('networkidle');
  const composer = page.locator('[data-main-composer="true"] [contenteditable="true"]');
  assert.equal(await composer.evaluate(el => el === document.activeElement), true, 'new conversation focuses input');
  await page.keyboard.press('Control+Alt+i');
  await composer.fill('Keep this draft');
  await page.keyboard.press('Control+Alt+n');
  await page.waitForFunction(() => document.querySelector('.ace-topbar-session-title')?.textContent.includes('Shortcut task 2'));
  await page.waitForLoadState('networkidle');
  await page.waitForFunction(() => ![...document.querySelectorAll('[data-ace-native-overlay="blocking"]')].some(el => el.getClientRects().length));
  await page.waitForTimeout(150);
  await page.keyboard.press('Control+Shift+-');
  await page.waitForFunction(() => document.querySelector('.ace-topbar-session-title')?.textContent.includes('Shortcut task 1'));
  await page.waitForFunction(() => document.querySelector('[data-main-composer="true"] [contenteditable="true"]')?.textContent.includes('Keep this draft'));
  await page.keyboard.press('Control+-');
  await page.waitForFunction(() => document.querySelector('.ace-topbar-session-title')?.textContent.includes('Shortcut task 2'));
  assert.equal(sessions.length, 2);
  await page.waitForLoadState('networkidle');
  const resumed = calls.filter(call => call.path.endsWith('/resume')).length;
  await page.keyboard.press('Control+-');
  assert.equal(calls.filter(call => call.path.endsWith('/resume')).length, resumed, 'forward boundary does not resume current session');
  pass('new session, input focus, draft preservation and requested history directions');

  const side = page.locator('.ace-topbar button[title*="Ctrl+Alt+B"]');
  const sideState = await side.getAttribute('aria-pressed');
  await page.keyboard.press('Control+Alt+b');
  assert.notEqual(await side.getAttribute('aria-pressed'), sideState);
  await page.keyboard.press('Control+Alt+b');
  const right = page.locator('.ace-topbar button[title*="Ctrl+Alt+E"]');
  const rightState = await right.getAttribute('aria-pressed');
  await page.keyboard.press('Control+Alt+e');
  assert.notEqual(await right.getAttribute('aria-pressed'), rightState);
  await page.keyboard.press('Control+Alt+e');
  pass('left and right panel actions');

  const selector = page.getByRole('button', { name: /^思考深度：/ });
  await selector.waitFor();
  await page.keyboard.press('Control+Alt+i');
  await page.keyboard.press('Control+Alt+Shift+>');
  await page.waitForFunction(() => document.querySelector('.ace-composer-reasoning-button')?.textContent.includes('最大'));
  const reasoningCalls = () => calls.filter(call => call.path.endsWith('/reasoning'));
  const ceiling = reasoningCalls().length;
  await page.keyboard.press('Control+Alt+.');
  assert.equal(reasoningCalls().length, ceiling);
  await page.keyboard.press('Control+Alt+,');
  await page.waitForFunction(() => document.querySelector('.ace-composer-reasoning-button')?.textContent.includes('高'));
  await composer.evaluate(el => {
    el.dispatchEvent(new CompositionEvent('compositionstart', { bubbles: true }));
    el.dispatchEvent(new KeyboardEvent('keydown', { code: 'Comma', key: '<', ctrlKey: true, altKey: true, bubbles: true, isComposing: true }));
    el.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true }));
  });
  assert.equal(reasoningCalls().length, ceiling + 1);
  await selector.click();
  await page.keyboard.press('Control+Alt+,');
  assert.equal(reasoningCalls().length, ceiling + 1);
  await page.keyboard.press('Escape');
  pass('reasoning physical/symbol keys, default depth, upper bound, IME and menu guards');

  const emit = async (type, payload, session_id) => page.evaluate(async ({ type, payload, session_id }) => {
    const { connection } = await import('/src/lib/connection.js');
    connection.dispatchEvent(new CustomEvent('message', { detail: { type, payload, session_id } }));
  }, { type, payload, session_id });
  await emit('busy_changed', { busy: true, turn_id: 'active-turn' }, 'shortcut-2');
  await page.keyboard.press('Control+Alt+,');
  await page.waitForFunction(() => document.querySelector('.ace-composer-reasoning-button')?.textContent.includes('低'));
  await page.keyboard.press('Control+Shift+.');
  await page.waitForTimeout(100);
  assert.equal(await page.evaluate(() => window.sentFrames.filter(frame => frame.type === 'abort').length), 1);
  await page.keyboard.press('Control+Shift+.');
  assert.equal(await page.evaluate(() => window.sentFrames.filter(frame => frame.type === 'abort').length), 1);
  await emit('busy_changed', { busy: false }, 'shortcut-2');
  pass('busy reasoning updates and stop once');

  const openConsole = page.getByRole('button', { name: '打开控制台 (Ctrl+`)', exact: true });
  if (await openConsole.count()) await page.keyboard.press('Control+Backquote');
  await page.getByRole('button', { name: '新建终端(默认 shell)', exact: true }).click();
  const terminal = page.locator('.xterm-helper-textarea');
  await terminal.waitFor({ state: 'attached' });
  await terminal.focus();
  await page.keyboard.press('Control+k');
  await page.locator('[data-search-palette]').waitFor();
  await page.keyboard.press('Control+k');
  await page.locator('[data-search-palette]').waitFor({ state: 'detached' });
  await terminal.focus();
  await page.keyboard.press('Control+Alt+i');
  assert.equal(await composer.evaluate(el => el === document.activeElement), true);
  await terminal.focus();
  await page.keyboard.press('Control+Backquote');
  await page.getByRole('button', { name: '打开控制台 (Ctrl+`)', exact: true }).waitFor();
  pass('terminal forwards app search, input focus and console toggle shortcuts');

  await page.keyboard.press('Control+k');
  await page.locator('[data-search-palette]').waitFor();
  await page.keyboard.press('Control+k');
  await page.locator('[data-search-palette]').waitFor({ state: 'detached' });
  await page.keyboard.press('Control+,');
  await page.locator('[data-settings-window]').waitFor();
  await page.keyboard.press('Control+/');
  await search.waitFor();
  await page.getByRole('button', { name: '消息通道', exact: true }).click();
  await page.keyboard.press('Control+/');
  await search.waitFor();
  await page.getByRole('button', { name: '消息通道', exact: true }).click();
  await page.locator('[data-channel-card]').first().waitFor();
  assert.equal(await page.locator('[data-channel-icon]').count(), 7);
  assert.equal(await page.locator('[data-channel-icon]').evaluateAll(images => images.every(img => img.complete && img.naturalWidth > 0)), true);
  const card = page.locator('[data-channel-card="telegram"]');
  const baseHeight = (await card.boundingBox()).height;
  const platform = channels.find(item => item.platform === 'telegram');
  for (const state of ['disabled', 'connecting', 'connected', 'retrying', 'failed', 'standby']) {
    Object.assign(platform, { configured: true, enabled: state !== 'disabled', state, owner: 'user:1', display_name: '@fixture_bot',
      pending: [{ id: 'request-1', principal: 'user:2', kind: 'user', name: 'Test user', expires_in_s: 120 }] });
    await emit('channels_state', platform);
    await page.waitForTimeout(30);
    assert.equal((await card.boundingBox()).height, baseHeight, state);
    assert.equal(await card.locator('[data-channel-connect]').innerText(), platform.enabled ? '取消连接' : '连接');
    const management = await card.locator('[data-channel-manage]').boundingBox();
    const connect = await card.locator('[data-channel-connect]').boundingBox();
    assert.ok(management.x + management.width <= connect.x);
  }
  await card.locator('[data-channel-manage]').click();
  await page.getByText('1 个请求待批准', { exact: true }).waitFor();
  await page.getByRole('button', { name: '批准', exact: true }).click();
  await page.getByText('1 个请求待批准', { exact: true }).waitFor({ state: 'detached' });
  await page.getByRole('button', { name: '完成', exact: true }).click();
  await card.locator('[data-channel-connect]').click();
  await page.waitForFunction(() => document.querySelector('[data-channel-card="telegram"] [data-channel-connect]')?.textContent.trim() === '连接');
  await card.locator('[data-channel-connect]').click();
  await page.waitForFunction(() => document.querySelector('[data-channel-card="telegram"] [data-channel-connect]')?.textContent.trim() === '取消连接');
  pass('all channel states preserve height, management precedes connection, approvals and reconnect work');

  for (const [width, theme] of [[1440, 'light'], [1440, 'dark'], [640, 'light'], [420, 'dark']]) {
    await page.setViewportSize({ width, height: 900 });
    await page.evaluate(theme => document.documentElement.setAttribute('data-theme', theme), theme);
    await page.waitForTimeout(80);
    assert.ok(await page.locator('[data-channel-card]').evaluateAll(cards => cards.every(el => el.scrollWidth <= el.clientWidth)), `channel overflow ${width}`);
    assert.ok(await page.locator('[data-channel-card] [title]').evaluateAll(elements => elements.filter(el => el.classList.contains('truncate')).every(el => el.clientWidth >= 60)), `platform names ${width}`);
    await page.screenshot({ path: path.join(output, `channels-${width}-${theme}.png`) });
  }
  await page.keyboard.press('Control+/');
  await search.waitFor();
  await search.fill('Ctrl');
  assert.equal(await page.locator('[data-shortcut-id]').count(), 15);
  await page.screenshot({ path: path.join(output, 'shortcuts-420-dark.png') });
  await page.evaluate(async () => { await (await import('/src/i18n/index.js')).applyLocalePreference('en-US'); });
  await search.fill('reasoning');
  assert.equal(await page.locator('[data-shortcut-id]').count(), 2);
  pass('narrow shortcuts and English name search');
  pass('desktop/narrow light/dark channel layouts and offline SVG assets');
  assert.deepEqual(errors, []);
  await fs.writeFile(path.join(output, 'results.json'), JSON.stringify({ checks, errors }, null, 2));
  console.log('Evidence:', output);
} catch (error) {
  await page?.screenshot({ path: path.join(output, 'failure.png') });
  console.error('Page state:', await page?.evaluate(() => ({ hash: location.hash,
    overlays: [...document.querySelectorAll('[role="dialog"], [role="menu"], [data-ace-native-overlay="blocking"], [data-shortcut-menu]')].map(el => ({ html: el.outerHTML.slice(0, 250), visible: el.getClientRects().length })),
    keys: window.keyRecords.slice(-12), text: document.body.innerText.slice(-1200) })));
  console.error('Writes:', calls.filter(call => call.method !== 'GET'));
  console.error('Runtime errors:', errors);
  console.error('Recent API requests:', calls.slice(-12));
  console.error('Evidence:', output);
  throw error;
} finally {
  await browser?.close();
  await server.close();
}
