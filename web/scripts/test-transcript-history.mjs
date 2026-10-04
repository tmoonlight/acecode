import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

// Real ChatView, isolated HTTP/WebSocket fixtures; never starts a daemon.
// Run from web with ACE_PLAYWRIGHT_MODULE / ACE_CHROMIUM_EXECUTABLE if needed.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = process.env.ACE_HISTORY_TEST_OUTPUT;
const entry = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {ChatView} from '/src/components/ChatView.jsx';
import {connection} from '/src/lib/connection.js';
import {applyLocalePreference} from '/src/i18n/index.js';
import '/src/styles/globals.css';
await applyLocalePreference('zh-CN');
const root=createRoot(document.getElementById('root')), empty=[];
let sid='history-main';
window.fixture={
 render(id){sid=id;root.render(<ChatView sessionRef={{sessionId:sid,title:'历史加载验证',workspaceHash:''}}
 health={{}} permissionRequests={empty} recentExpertIds={empty} messageAutoCollapse={false}/>);},
 event(type,payload){connection.dispatchEvent(new CustomEvent('message',{detail:{type,session_id:sid,payload}}));},
 async locale(value){await applyLocalePreference(value);window.fixture.render(sid);},
};
window.fixture.render(sid);
`;
const html = `<!doctype html><html><head><meta charset="UTF-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/></head>
<body><div id="root" style="height:100vh;display:flex;width:100%;background:var(--ace-bg);color:var(--ace-fg)"></div>
<script>class FixtureWebSocket extends EventTarget {static OPEN=1;static CLOSED=3;constructor(){super();this.readyState=1;setTimeout(()=>this.dispatchEvent(new Event('open')),0);}send(){}close(){this.readyState=3;}}window.WebSocket=FixtureWebSocket;</script>
<script type="module" src="/@id/virtual:history-fixture"></script></body></html>`;
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), server: { host: '127.0.0.1', port: 0 },
  plugins: [{ name: 'history-fixture', enforce: 'pre',
    resolveId(id) { if (id === 'virtual:history-fixture') return '\0virtual:history-fixture'; },
    async load(id) { if (id === '\0virtual:history-fixture') return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx', jsx: 'automatic' })).code; },
    configureServer(vite) { vite.middlewares.use('/history-fixture', async (_req, res) => {
      res.setHeader('Content-Type', 'text/html'); res.end(await vite.transformIndexHtml('/history-fixture', html));
    }); },
  }],
});
const messages = (start, count) => Array.from({ length: count }, (_, n) => {
  const i = start + n;
  return { id: `message-${i}`, role: i % 2 ? 'assistant' : 'user', message_position: String(i),
    content: i % 2 ? `第 ${i} 条回复\n\n用于验证历史阅读位置。\n\n`.repeat(3) : `第 ${i} 条问题`, timestamp: '2026-10-04T01:00:00Z' };
});
const checks = [], errors = [], pending = [], requests = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1325, height: 775 } });
  page.on('pageerror', error => errors.push(error.message));
  await page.route('**/api/**', async route => {
    const url = new URL(route.request().url());
    let data = {};
    if (url.pathname.endsWith('/messages')) {
      requests.push(url.href);
      if (url.searchParams.has('before')) {
        await new Promise(resolve => pending.push(async (body, status = 200) => {
          await route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) }); resolve();
        }));
        return;
      }
      const local = url.pathname.includes('history-local');
      data = { messages: local ? messages(0, 300) : messages(100, 12), events: [], status: 'idle', has_more: !local, before: 'page-100' };
    } else if (url.pathname.includes('models')) data = { models: [], profiles: [] };
    else if (url.pathname.includes('workspaces')) data = { workspaces: [] };
    else if (url.pathname.includes('history')) data = { items: [], history: [] };
    else if (url.pathname.includes('commands')) data = { commands: [] };
    else if (url.pathname.includes('skills')) data = { skills: [] };
    else if (url.pathname.includes('experts')) data = { experts: [] };
    else if (url.pathname.includes('subagents')) data = { tasks: [] };
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(data) });
  });
  const viewport = page.locator('.ace-chat-transcript-scroll');
  const boundary = page.locator('[data-transcript-history-boundary]');
  const wait = () => page.waitForTimeout(180);
  const busy = () => page.waitForFunction(() => document.querySelector('[data-transcript-history-boundary]')?.getAttribute('aria-busy') === 'true');
  const idle = () => page.waitForFunction(() => document.querySelector('[data-transcript-history-boundary]')?.getAttribute('aria-busy') !== 'true');
  const check = (name, condition, detail) => {
    assert.ok(condition, `${name}: ${JSON.stringify(detail)}`); checks.push(name); console.log(`[pass] ${name}`);
  };
  const wheel = async delta => {
    const box = await viewport.boundingBox();
    await page.mouse.move(box.x + box.width / 2, box.y + Math.min(170, box.height / 2));
    await page.mouse.wheel(0, delta);
  };
  const top = async () => { await viewport.evaluate(el => { el.scrollTop = 0; }); await wait(); };
  const rowTop = () => page.locator('[data-chat-message-position="100"]').first().evaluate(el => el.getBoundingClientRect().top);
  const finish = async (body, status) => {
    await page.waitForFunction(() => document.querySelector('[data-transcript-history-boundary]')?.getAttribute('aria-busy') === 'true');
    for (let i = 0; i < 30 && pending.length === 0; i++) await page.waitForTimeout(20);
    assert.ok(pending.length, 'page request must have reached the fixture');
    await pending.shift()(body, status); await idle(); await wait();
  };
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/history-fixture`);
  await page.locator('[data-chat-message-position="100"]').waitFor(); await wait();
  check('initial load requests only the tail', requests.length === 1);
  await top();
  check('programmatic top does not request history', pending.length === 0);
  await wheel(-100); await busy(); await wait();
  check('loading is inline with no full-screen mask', await boundary.getByText('正在加载更早的消息…').count() === 1
    && await page.locator('[data-session-content-loading]').count() === 0);
  check('show all is disabled during paging', await boundary.getByRole('button', { name: '显示全部' }).isDisabled());
  const anchorBefore = await rowTop();
  await page.evaluate(() => window.fixture.event('token', { text: '\n\n实时新增内容。\n\n'.repeat(30) })); await wait();
  await wheel(-100); await wheel(-100); await wait();
  check('streaming and repeated wheel keep one page request', pending.length === 1 && Math.abs(await rowTop() - anchorBefore) < 2);
  if (output) { await mkdir(output, { recursive: true }); await page.screenshot({ path: path.join(output, 'inline-loading.png') }); }
  await finish({ messages: messages(80, 20), has_more: true, before: 'page-80' });
  check('prepend preserves the reading row during streaming', Math.abs(await rowTop() - anchorBefore) < 2, { before: anchorBefore, after: await rowTop() });
  await page.locator('[data-chat-message-position="80"]').evaluate(el => {
    const img = document.createElement('img'); img.alt = ''; img.style.height = '1px'; img.style.width = '10px';
    el.appendChild(img); setTimeout(() => { img.style.height = '220px'; }, 40);
  }); await wait();
  check('delayed image height preserves the reading row', Math.abs(await rowTop() - anchorBefore) < 2);
  check('layout does not drain another page', pending.length === 0);

  await page.getByRole('button', { name: '滚动到底部', exact: true }).click(); await wait();
  await top(); await wheel(-100); await busy();
  await finish({ error: 'OFFLINE', message: 'fixture failure' }, 503);
  check('error stays at boundary with retry and no mask', await boundary.getByText('更早的消息加载失败').count() === 1
    && await boundary.getByRole('button', { name: '重试', exact: true }).count() === 1
    && await page.locator('[data-session-content-loading]').count() === 0);
  await wheel(-100); await wait();
  check('failure does not retry automatically', pending.length === 0);
  await boundary.getByRole('button', { name: '重试', exact: true }).click(); await busy();
  await finish({ messages: messages(60, 20), has_more: true, before: 'page-60' });
  check('explicit retry works', await page.locator('[data-chat-message-position="60"]').count() === 1);

  await page.getByRole('button', { name: '滚动到底部', exact: true }).click(); await wait();
  await top(); await wheel(-100); await busy(); await wait();
  await page.evaluate(() => window.fixture.render('history-next')); await wait();
  check('new session clears old paging state', await boundary.getAttribute('aria-busy') === 'false');
  await pending.shift()({ messages: messages(0, 30), has_more: false }); await wait();
  check('late page cannot reveal rows in another session', await page.locator('[data-chat-message-position="0"]').count() === 0);

  await page.evaluate(() => window.fixture.render('history-local')); await wait();
  await page.waitForFunction(() => document.querySelector('[data-transcript-history-boundary]')?.textContent.includes('条消息'));
  const localBefore = await page.locator('[data-chat-row="true"]').count();
  const requestCount = requests.length;
  await top(); await wheel(-100); await idle(); await wait();
  check('local hidden rows reveal without a network request', await page.locator('[data-chat-row="true"]').count() > localBefore && requests.length === requestCount);

  await page.evaluate(() => window.fixture.render('history-user-scroll')); await wait();
  await top(); await wheel(-100); await busy();
  await wheel(30); await wait();
  const latestReadingTop = await rowTop();
  await finish({ messages: messages(80, 20), has_more: true, before: 'page-80' });
  check('scrolling while waiting preserves the new reading position', Math.abs(await rowTop() - latestReadingTop) < 2);
  await page.getByRole('button', { name: '滚动到底部', exact: true }).click(); await wait();
  await top(); await wheel(-100); await busy();
  await page.getByRole('button', { name: '滚动到底部', exact: true }).click(); await wait();
  await finish({ messages: messages(60, 20), has_more: true, before: 'page-60' });
  check('return to tail during loading overrides history anchoring', await viewport.evaluate(el => el.scrollHeight - el.clientHeight - el.scrollTop <= 2));

  await page.evaluate(() => window.fixture.render('history-keyboard')); await wait();
  await viewport.evaluate(el => { el.tabIndex = 0; el.focus(); });
  await page.keyboard.press('Control+Home'); await busy();
  await finish({ messages: [], has_more: true, before: 'page-100' });
  check('keyboard top triggers one page and no-progress offers retry', await boundary.getByText('更早的消息加载失败').count() === 1 && pending.length === 0);
  await boundary.getByRole('button', { name: '重试', exact: true }).click(); await busy();
  await finish({ error: 'HISTORY_CURSOR_STALE' }, 409);
  check('stale cursor reload stays inline and stops automatic paging', await boundary.getByText('会话历史已更新，请重试').count() === 1
    && await page.locator('[data-session-content-loading]').count() === 0 && pending.length === 0);

  await page.evaluate(() => window.fixture.render('history-all')); await wait();
  await top(); await boundary.getByRole('button', { name: '显示全部' }).click(); await busy(); await wait();
  const allAnchorTop = await rowTop();
  await pending.shift()({ messages: messages(80, 20), has_more: true, before: 'page-80' });
  await wait();
  check('explicit show all keeps a single inline operation across pages', await boundary.getAttribute('aria-busy') === 'true' && pending.length === 1);
  await finish({ messages: messages(60, 20), has_more: false });
  check('show all preserves position and removes exhausted boundary', Math.abs(await rowTop() - allAnchorTop) < 2 && await boundary.count() === 0);

  await page.evaluate(() => window.fixture.render('history-mobile')); await wait();
  await page.setViewportSize({ width: 390, height: 844 });
  await page.evaluate(() => { document.documentElement.dataset.theme = 'dark'; });
  await top(); await wheel(-100); await busy(); await wait();
  check('narrow dark loading stays within the viewport', await boundary.evaluate(el => el.getBoundingClientRect().right <= innerWidth)
    && await page.locator('[data-session-content-loading]').count() === 0);
  if (output) await page.screenshot({ path: path.join(output, 'inline-loading-narrow-dark.png') });
  await finish({ messages: messages(0, 12), has_more: false });
  check('exhausted history removes the boundary', await boundary.count() === 0);
  // The application's locale change rebuilds memoized API clients. Verify an
  // English paging operation in its new scope, not a cancelled Chinese request.
  await page.evaluate(async () => { await window.fixture.locale('en-US'); window.fixture.render('history-english'); });
  await wait(); await top(); await wheel(-100); await busy(); await wait();
  check('loading has English localization', await boundary.getByText('Loading earlier messages…').count() === 1,
    { text: await boundary.textContent(), locale: await page.evaluate(() => document.documentElement.lang) });
  await finish({ messages: messages(0, 12), has_more: false });
  check('no browser runtime errors', errors.length === 0, errors);
} finally {
  if (output) {
    await mkdir(output, { recursive: true });
    await writeFile(path.join(output, 'results.json'), JSON.stringify({ checks, errors }, null, 2));
  }
  await browser?.close(); await server.close();
}
