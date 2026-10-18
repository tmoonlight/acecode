import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

// Mount the production App (including TopBar, Sidebar, ChatView and persistence).
// Network fixtures are isolated from the user's daemon and browser profile.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = process.env.ACE_RESPONSIVE_TEST_OUTPUT || await fs.mkdtemp(path.join(os.tmpdir(), 'ace-responsive-sidebars-'));
await fs.mkdir(output, { recursive: true });
const server = await createServer({ root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false } });
const session = { id: 'resize-session', session_id: 'resize-session', title: 'Resize fixture',
  cwd: 'C:/fixture', workspace_hash: 'fixture', active: true, updated_at: '2026-10-17T10:00:00Z' };
const messages = [
  { id: 'user-1', role: 'user', content: '请检查工作台缩放时的对话阅读体验。' },
  { id: 'assistant-1', role: 'assistant', content: '这里是用于验证布局的对话正文。窗口变窄时，对话应优先保留足够的阅读空间。\n\n打开 [source.js](source.js) 查看文件预览。\n\n' + '继续阅读这段内容，侧栏自动收起后，消息和输入框仍应完整可见。'.repeat(12) },
];
let browser;
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.setDefaultTimeout(30000);
  page.on('pageerror', e => { errors.push(e.message); console.error('[pageerror]', e.message); });
  await page.addInitScript(() => {
    class FixtureWebSocket extends EventTarget {
      static OPEN = 1; static CLOSED = 3;
      constructor() { super(); this.readyState = 1; setTimeout(() => { this.onopen?.({}); this.dispatchEvent(new Event('open')); }, 0); }
      send() {} close() { this.readyState = 3; }
    }
    window.WebSocket = FixtureWebSocket;
    if (!localStorage.getItem('responsive-fixture-seeded')) {
      localStorage.setItem('acecode.uiPrefs.v1', JSON.stringify({ view: 'single', sidebarCollapsed: false,
        sidePanelCollapsed: false, sidePanelListCollapsed: false, fontSize: 'medium' }));
      localStorage.setItem('responsive-fixture-seeded', 'true');
    }
  });
  await page.route('**/api/**', async route => {
    const { pathname, searchParams } = new URL(route.request().url());
    let data = {};
    if (pathname === '/api/health') data = { status: 'ok', version: '1.0.0', cwd: 'C:/fixture' };
    else if (pathname === '/api/config/ui-preferences') data = { theme: 'light', color_theme: 'blue', font_size: 'medium' };
    else if (pathname === '/api/workspaces') data = [{ hash: 'fixture', cwd: 'C:/fixture', name: 'Resize project' }];
    else if (pathname.endsWith('/messages')) data = { messages, events: [], busy: false, title: session.title, cwd: session.cwd };
    else if (pathname.endsWith('/sessions') || pathname.endsWith('/session-sidebar')) data = pathname === '/api/session-sidebar' ? []
      : searchParams.has('parent') ? [{ ...session, id: 'resize-child', session_id: 'resize-child', title: 'Child transcript', parent_session_id: searchParams.get('parent') }] : [session];
    else if (pathname.endsWith('/resume') || /^\/api\/sessions\/[^/]+$/.test(pathname)) {
      const id = pathname.match(/\/sessions\/([^/]+)/)?.[1] || session.id;
      data = { ...session, id, session_id: id };
    }
    else if (pathname.endsWith('/model')) data = { name: 'fixture', provider: 'openai', model: 'test' };
    else if (pathname.includes('models')) data = { models: [], profiles: [] };
    else if (pathname.includes('history')) data = { items: [], history: [] };
    else if (pathname.includes('commands')) data = { commands: [] };
    else if (pathname.includes('skills')) data = { skills: [] };
    else if (pathname.includes('experts')) data = { experts: [] };
    else if (pathname.includes('subagents')) data = { tasks: [] };
    else if (pathname.includes('loops') || pathname.includes('/pty') || pathname.includes('/themes')) data = [];
    else if (pathname === '/api/files') data = [{ name: 'source.js', path: 'source.js', kind: 'file' }];
    else if (pathname.includes('/files/content')) {
      await route.fulfill({ status: 200, contentType: 'text/plain', body: 'export const answer = 42;\n' }); return;
    } else if (pathname.includes('/files/edit')) data = { text: 'export const answer = 42;\n', read_id: 'fixture-read' };
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(data) });
  });
  const settle = async () => {
    await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
    await page.waitForTimeout(300);
  };
  const geometry = () => page.evaluate(() => {
    const rect = selector => {
      const e = document.querySelector(selector); const r = e?.getBoundingClientRect();
      return { width: r?.width || 0, x: r?.x || 0, right: r?.right || 0, visible: !!e && r.width > 0 && r.height > 0 && getComputedStyle(e).visibility !== 'hidden' };
    };
    return { left: rect('.ace-responsive-sidebar-left'), right: rect('.ace-responsive-sidebar-right'),
      transcript: rect('[data-conversation-find-root]'), chat: rect('.ace-session-panel'),
      popup: document.querySelector('[data-sidebar-popup]')?.getAttribute('data-sidebar-popup') || null,
      overflow: document.documentElement.scrollWidth > innerWidth };
  });
  const prefs = () => page.evaluate(async () => {
    const { sessionWorkbench } = await import('/src/lib/sessionWorkbench.js');
    sessionWorkbench.flush();
    return { global: JSON.parse(localStorage.getItem('acecode.uiPrefs.v1')),
      panels: sessionWorkbench.get('session:resize-session', 'panels', {}),
      widths: localStorage.getItem('acecode.singleLayoutWidths.v1') };
  });
  const resize = async width => { await page.setViewportSize({ width, height: 1080 }); await settle(); return geometry(); };
  const left = page.locator('[data-sidebar-toggle="left"]');
  const right = page.locator('[data-sidebar-toggle="right"]');
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/`, { waitUntil: 'commit' });
  await page.locator('.ace-sidebar-session-title-button').filter({ hasText: 'Resize fixture' }).first().click();
  await page.locator('[data-conversation-find-root]').waitFor();
  await settle();
  if (!(await geometry()).right.visible) { await right.click(); await settle(); }
  let g;
  if (!process.argv.includes('--popup-resize-only')) {
  const initial = await prefs();
  assert.equal((await geometry()).left.visible, true);
  assert.equal((await geometry()).right.visible, true);
  await page.screenshot({ path: path.join(output, '1920-wide.png') });
  g = await resize(906);
  assert.equal(g.left.visible, true); assert.equal(g.right.visible, true); assert.equal(g.transcript.width, 350);
  g = await resize(905);
  assert.equal(g.left.visible, true); assert.equal(g.right.visible, false);
  g = await resize(969); assert.equal(g.right.visible, false);
  g = await resize(970); assert.equal(g.right.visible, true);
  g = await resize(626);
  assert.equal(g.left.visible, true); assert.equal(g.right.visible, false); assert.equal(g.transcript.width, 350);
  g = await resize(625); assert.equal(g.left.visible, false);
  g = await resize(689); assert.equal(g.left.visible, false);
  g = await resize(690); assert.equal(g.left.visible, true);
  g = await resize(600);
  assert.equal(g.left.visible, false); assert.equal(g.right.visible, false); assert.ok(g.transcript.width >= 350, JSON.stringify(g));
  await page.screenshot({ path: path.join(output, '600-transcript.png') });
  for (const side of ['left', 'right']) {
    const button = side === 'left' ? left : right;
    const before = await geometry(); await button.click(); await settle(); g = await geometry();
    assert.equal(g.popup, side); assert.equal(g.transcript.width, before.transcript.width);
    await page.locator(`[data-sidebar-popup="${side}"]`).click({ position: { x: 30, y: 10 } });
    assert.equal((await geometry()).popup, side);
    await page.locator('[data-conversation-find-root]').click({ position: { x: side === 'left' ? 450 : 20, y: 180 } });
    await settle(); assert.equal((await geometry()).popup, null);
    await button.click(); await settle(); await page.keyboard.press('Escape'); await settle();
    assert.equal((await geometry()).popup, null); assert.equal(await button.evaluate(e => e === document.activeElement), true);
    if (side === 'right') {
      await right.click(); await settle();
      await page.locator('[data-sidebar-popup="right"]').getByRole('button', { name: '收起列表面板', exact: true }).click();
      await settle(); assert.equal((await geometry()).popup, null);
      assert.equal(await right.getAttribute('aria-expanded'), 'false');
    }
  }
  await left.click(); await settle(); await right.click(); await settle(); assert.equal((await geometry()).popup, 'right');
  await right.click(); await settle(); assert.equal((await geometry()).popup, null);
  assert.deepEqual(await prefs(), initial);
  g = await resize(1920); assert.equal(g.left.visible, true); assert.equal(g.right.visible, true);
  console.log('[pass] production App collapses right then left, popup does not squeeze transcript, outside/Escape/toggle/mutual exclusion preserve preferences');

  for (const leftClosed of [false, true]) for (const rightClosed of [false, true]) {
    g = await geometry(); if (g.left.visible === leftClosed) await left.click(); if (g.right.visible === rightClosed) await right.click(); await settle();
    const saved = await prefs();
    await resize(600); await left.click(); await settle(); await right.click(); await settle();
    await resize(1920); g = await geometry();
    assert.equal(g.left.visible, !leftClosed, JSON.stringify({ g, prefs: await prefs(), saved }));
    assert.equal(g.right.visible, !rightClosed, JSON.stringify({ g, prefs: await prefs(), saved }));
    assert.deepEqual(await prefs(), saved);
    await resize(600);
    await page.evaluate(() => history.replaceState(null, '', '/?open=resize-session&workspace=fixture'));
    await page.reload({ waitUntil: 'commit' });
    await page.locator('[data-conversation-find-root]').waitFor(); await settle();
    await resize(1920); g = await geometry();
    assert.equal(g.left.visible, !leftClosed); assert.equal(g.right.visible, !rightClosed);
    assert.deepEqual(await prefs(), saved);
  }
  console.log('[pass] all four open/closed combinations survive popup, resize and reload');

  // Opening a real file changes the right-hand width requirement without resize.
  await left.click(); await right.click(); await settle();
  // Exercise the shared Desktop editor in Chromium without starting a native shell.
  await page.evaluate(() => { window.__ACECODE_DESKTOP_SHELL__ = true; });
  await page.locator('[data-file-path="source.js"]').first().click();
  await page.locator('.ace-preview-details-shell').waitFor(); await settle();
  const editor = page.locator('.ace-file-text-editor');
  await editor.waitFor(); await editor.fill('export const edited = 123;\n');
  await editor.evaluate(e => { e.setSelectionRange(5, 10); window.responsiveSavedEditor = e; });
  const beforePreviewResize = await prefs();
  g = await resize(1300); assert.equal(g.right.visible, false); assert.ok(g.transcript.width >= 350);
  await right.click(); await settle();
  assert.equal((await geometry()).popup, 'right');
  assert.equal(await editor.inputValue(), 'export const edited = 123;\n');
  assert.equal(await editor.evaluate(e => e === window.responsiveSavedEditor), true);
  await page.screenshot({ path: path.join(output, '1300-preview-popup.png') });
  g = await resize(800); assert.equal(g.popup, 'right'); assert.ok(g.right.right <= 800);
  await resize(600); await editor.waitFor();
  const expandList = page.locator('.ace-preview-details-panel').getByRole('button', { name: '展开列表面板', exact: true });
  await expandList.click(); await settle();
  assert.equal(await page.locator('.ace-popup-navigation').isVisible(), true);
  await page.locator('.ace-popup-navigation').getByRole('button', { name: '收起列表面板', exact: true }).click();
  await settle(); assert.equal((await geometry()).popup, 'right');
  assert.equal(await editor.isVisible(), true);
  await page.locator('[data-conversation-find-root]').click({ position: { x: 5, y: 150 } }); await settle();
  assert.equal((await geometry()).popup, null);
  g = await resize(1920); assert.equal(g.right.visible, true);
  assert.equal(await editor.inputValue(), 'export const edited = 123;\n');
  assert.equal(await editor.evaluate(e => e === window.responsiveSavedEditor), true);
  assert.deepEqual(await prefs(), beforePreviewResize);
  console.log('[pass] real preview width, compact popup navigation, editor DOM and unsaved draft survive responsive transitions');

  // A left-sidebar menu is a React portal outside the popup's DOM subtree.
  await resize(600); await left.click(); await settle();
  await page.getByRole('button', { name: 'ACECode 快捷菜单', exact: true }).click();
  const menu = page.locator('[data-shortcut-menu="true"]'); await menu.waitFor();
  await menu.dispatchEvent('pointerdown', { bubbles: true, pointerId: 1 });
  assert.equal((await geometry()).popup, 'left');
  await page.keyboard.press('Escape'); await settle(); assert.equal((await geometry()).popup, 'left');
  await page.keyboard.press('Escape'); await settle(); assert.equal((await geometry()).popup, null);
  console.log('[pass] portal menu interaction and nested Escape keep the sidebar alive until the outer dismissal');

  for (const theme of ['light', 'dark']) {
    await page.evaluate(theme => document.documentElement.dataset.theme = theme, theme);
    for (const width of [1920, 1100, 800, 320]) {
      g = await resize(width); assert.equal(g.overflow, false, JSON.stringify(g));
      assert.ok(g.transcript.width >= Math.min(350, width - 6) - 1, JSON.stringify(g));
      if (width === 320) {
        await right.click(); await settle(); g = await geometry();
        assert.ok(g.right.right <= width && g.right.x >= 0, JSON.stringify(g));
      }
      await page.screenshot({ path: path.join(output, `${theme}-${width}.png`) });
      if (g.popup) { await page.keyboard.press('Escape'); await settle(); }
    }
  }
  console.log('[pass] light/dark at 1920, 1100, 800 and 320 CSS pixels keep transcript and popup inside the viewport');

  await resize(1920);
  await page.getByRole('button', { name: '最大化预览面板', exact: true }).click(); await settle();
  assert.equal((await geometry()).transcript.visible, false);
  await resize(1200); assert.equal(await editor.isVisible(), true);
  await page.getByRole('button', { name: '还原预览面板', exact: true }).click(); await settle();
  assert.equal((await geometry()).transcript.visible, true);
  await resize(1920); await editor.fill('export const answer = 42;\n'); await settle();
  await page.getByRole('button', { name: '隐藏详情面板', exact: true }).click(); await settle();
  await resize(1000);
  await page.getByRole('button', { name: '后台任务', exact: true }).click(); await settle();
  g = await geometry(); assert.equal(g.right.visible, false); assert.equal(g.left.visible, false);
  assert.ok(g.transcript.width >= 350, JSON.stringify(g));
  await page.getByRole('button', { name: '后台任务', exact: true }).click(); await settle();
  console.log('[pass] explicit preview maximization remains usable and child transcript pressure collapses sidebars');

  const originalOwnerPrefs = await prefs();
  await resize(600); await right.click(); await settle();
  await page.evaluate(() => window.aceDesktop_focusSessionFromBridge('resize-other', 'fixture'));
  await settle(); assert.equal((await geometry()).popup, null);
  assert.deepEqual(await prefs(), originalOwnerPrefs);
  await page.evaluate(() => window.aceDesktop_focusSessionFromBridge('resize-session', 'fixture'));
  await settle(); assert.equal((await geometry()).popup, null);
  console.log('[pass] changing workbench owners discards temporary popup without changing the original owner preferences');
  }

  // Exercise the actual popup edges; floating preferences must never change
  // the geometry used to decide whether a sidebar can dock.
  await resize(600);
  const resizeBase = await prefs();
  const withoutPopupWidths = value => {
    const widths = JSON.parse(value.widths);
    delete widths.sidebarPopup; delete widths.rightPanelPopup;
    return { ...value, widths };
  };
  const dragPopup = async (side, delta) => {
    const before = await geometry();
    const handle = page.locator(`[data-popup-resize="${side}"]`);
    const box = await handle.boundingBox();
    const x = box.x + box.width / 2, y = box.y + 160;
    await page.mouse.move(x, y); await page.mouse.down();
    await page.mouse.move(x + delta, y, { steps: 8 }); await settle();
    const during = await geometry();
    assert.equal(during.popup, side);
    assert.equal(during.transcript.width, before.transcript.width);
    await page.mouse.up(); await settle();
    assert.equal(await page.evaluate(() => document.body.classList.contains('ace-resizing')), false);
    return geometry();
  };
  await left.click(); await settle();
  const initialLeftWidth = (await geometry()).left.width;
  g = await dragPopup('left', 170); assert.equal(g.left.width, initialLeftWidth + 170);
  await left.click(); await left.click(); await settle();
  assert.equal((await geometry()).left.width, initialLeftWidth + 170);
  g = await dragPopup('left', -700); assert.equal(g.left.width, 160); assert.equal(g.popup, 'left');
  await page.locator('[data-popup-resize="left"]').focus(); await page.keyboard.press('ArrowRight'); await settle();
  assert.equal((await geometry()).left.width, 172);
  await dragPopup('left', 400);
  const wideLeftPrefs = await prefs();
  g = await resize(480); assert.equal(g.left.width, 456);
  assert.deepEqual(await prefs(), wideLeftPrefs);
  g = await dragPopup('left', -20); assert.equal(g.left.width, 436, 'drag starts at the rendered, clamped edge');
  g = await resize(600); assert.equal(g.left.width, 436);
  await page.screenshot({ path: path.join(output, 'left-popup-resized.png') });
  await left.click(); await settle();
  console.log('[pass] left popup supports drag/keyboard, remains floating even at 160px, and starts clamped drags without jumping');

  await right.click(); await settle();
  const initialRightWidth = (await geometry()).right.width;
  g = await dragPopup('right', -120); assert.equal(g.right.width, initialRightWidth + 120); assert.equal(g.right.right, 600);
  assert.equal(await page.locator('.ace-side-panel-shell').evaluate(e => e.getBoundingClientRect().width), g.right.width);
  await page.locator('[data-popup-resize="right"]').focus(); await page.keyboard.press('ArrowLeft'); await settle();
  const filePopupWidth = (await geometry()).right.width;
  assert.equal(filePopupWidth, initialRightWidth + 132);
  await right.click(); await right.click(); await settle(); assert.equal((await geometry()).right.width, filePopupWidth);
  console.log('[pass] navigation-only popup expands its content with the outer frame and remembers the selected width');

  await page.evaluate(() => { window.__ACECODE_DESKTOP_SHELL__ = true; });
  await page.locator('[data-file-path="source.js"]').first().click();
  const resizeEditor = page.locator('.ace-file-text-editor'); await resizeEditor.waitFor();
  await resizeEditor.fill('export const resizedDraft = 321;\n');
  await resizeEditor.evaluate(e => { window.popupResizeEditor = e; });
  g = await resize(1300);
  const detailStartWidth = g.right.width;
  g = await dragPopup('right', -160); assert.equal(g.right.width, detailStartWidth + 160); assert.equal(g.right.right, 1300);
  assert.equal(await resizeEditor.evaluate(e => e === window.popupResizeEditor), true);
  assert.equal(await resizeEditor.inputValue(), 'export const resizedDraft = 321;\n');
  await page.screenshot({ path: path.join(output, 'detail-popup-resized.png') });
  assert.deepEqual(withoutPopupWidths(await prefs()), withoutPopupWidths(resizeBase));

  // Closing or cancelling in the middle of a captured drag releases all state.
  let handle = page.locator('[data-popup-resize="right"]');
  let box = await handle.boundingBox();
  await page.mouse.move(box.x + 4, box.y + 160); await page.mouse.down();
  await page.keyboard.press('Escape'); await settle();
  assert.equal((await geometry()).popup, null);
  assert.equal(await page.evaluate(() => document.body.classList.contains('ace-resizing')), false);
  const afterClose = await prefs(); await page.mouse.move(box.x - 80, box.y + 160); await page.mouse.up(); await settle();
  assert.deepEqual(await prefs(), afterClose);
  await right.click(); await settle();
  handle = page.locator('[data-popup-resize="right"]'); box = await handle.boundingBox();
  await handle.evaluate(e => e.addEventListener('pointerdown', event => { window.popupDragPointer = event.pointerId; }, { once: true }));
  await page.mouse.move(box.x + 4, box.y + 160); await page.mouse.down();
  await handle.dispatchEvent('pointercancel', { bubbles: true, pointerId: await page.evaluate(() => window.popupDragPointer) });
  assert.equal(await page.evaluate(() => document.body.classList.contains('ace-resizing')), false);
  const afterCancel = await prefs(); await page.mouse.move(box.x - 80, box.y + 160); await page.mouse.up(); await settle();
  assert.deepEqual(await prefs(), afterCancel); assert.equal((await geometry()).popup, 'right');
  console.log('[pass] preview drag preserves transcript and draft; Escape and pointercancel release capture and listeners');

  const savedPopupPrefs = await prefs();
  const savedPopupWidth = (await geometry()).right.width;
  g = await resize(480); assert.equal(g.right.width, 456); assert.deepEqual(await prefs(), savedPopupPrefs);
  g = await resize(900); assert.equal(g.right.width, savedPopupWidth);
  await page.evaluate(() => history.replaceState(null, '', '/?open=resize-session&workspace=fixture'));
  await page.reload({ waitUntil: 'commit' }); await page.locator('[data-conversation-find-root]').waitFor(); await settle();
  await right.click(); await settle(); assert.equal((await geometry()).right.width, savedPopupWidth);
  assert.deepEqual(await prefs(), savedPopupPrefs);
  await resize(600);
  await left.click(); await settle(); assert.equal((await geometry()).left.width, 436);
  g = await resize(1920); assert.equal(g.popup, null); assert.equal(g.left.width, 270);
  assert.deepEqual(withoutPopupWidths(await prefs()), withoutPopupWidths(resizeBase));
  console.log('[pass] popup widths persist across reload and temporary viewport clamps without changing docked widths or open/closed preferences');
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ output, status: 'passed' }));
} catch (error) {
  if (browser) {
    const page = browser.contexts()[0]?.pages()[0];
    if (page) { await page.screenshot({ path: path.join(output, 'failure.png') });
      await fs.writeFile(path.join(output, 'failure.html'), await page.content()); }
  }
  console.error('Artifacts:', output); throw error;
} finally { await browser?.close(); server.httpServer?.closeAllConnections(); await server.close(); }
