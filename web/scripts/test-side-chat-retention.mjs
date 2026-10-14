import assert from 'node:assert/strict';
import { mkdtemp } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await mkdtemp(path.join(os.tmpdir(), 'ace-side-chat-retention-'));
const entry = `
import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import {ChatView} from '/src/components/ChatView.jsx';
import {SettingsPage} from '/src/components/SettingsPage.jsx';
import {DesktopContextMenu} from '/src/components/DesktopContextMenu.jsx';
import {ThemeProvider, useTheme} from '/src/theme.jsx';
import {Toaster} from '/src/components/Toast.jsx';
import {i18n} from '/src/i18n/index.js';
import '/src/styles/globals.css';
function Harness() {
  const [sid, setSid] = useState('retention-a');
  const [mounted, setMounted] = useState(true);
  const [settings, setSettings] = useState(false);
  const theme = useTheme();
  window.fixture = {switch: setSid, mount: setMounted, settings: setSettings,
    theme: theme.set, locale: value => i18n.changeLanguage(value),
    advance: ms => { window.sideChatNow += ms; }};
  return <>
    <div data-fixture-session={sid} className="ace-app-shell font-sans bg-bg text-fg" style={{height:'100vh',display:'flex'}}>
      {mounted && <ChatView sessionRef={{sessionId:sid,workspaceHash:'retention-qa'}} health={{}}/>}
    </div>
    {settings && <SettingsPage health={{}} onClose={()=>setSettings(false)}/>}
    <DesktopContextMenu/><Toaster/>
  </>;
}
createRoot(document.getElementById('root')).render(<React.StrictMode><ThemeProvider><Harness/></ThemeProvider></React.StrictMode>);
`;
const html = '<!doctype html><html><head><meta charset="UTF-8"/></head><body><div id="root"></div><script type="module" src="/@id/virtual:side-chat-retention"></script></body></html>';
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'side-chat-retention-fixture',
    resolveId(id) { if (id === 'virtual:side-chat-retention') return '\0virtual:side-chat-retention'; },
    async load(id) {
      if (id === '\0virtual:side-chat-retention') {
        return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx' })).code;
      }
    },
    configureServer(vite) {
      vite.middlewares.use('/side-chat-retention', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/side-chat-retention', html));
      });
    },
  }],
});
const errors = [];
const sockets = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 }, locale: 'zh-CN' });
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => {
    // Playwright's WebSocket mock uses URL.parse, added after Chromium 110.
    // Keep that test transport usable on the older Desktop WebView baseline.
    if (!URL.parse) URL.parse = (input, base) => {
      try { return new URL(input, base); } catch { return null; }
    };
    window.sideChatNow = Date.now();
    Date.now = () => window.sideChatNow;
  });
  await page.route('**/api/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body = {};
    if (pathname.endsWith('/messages')) body = { busy: false, messages: [
      { id: 'user', role: 'user', content: '主会话消息' },
      { id: 'answer', role: 'assistant', content: '主会话回答' },
    ], events: [] };
    else if (pathname.endsWith('/workspaces')) body = { workspaces: [] };
    else if (pathname.endsWith('/commands')) body = { builtins: [{ name: 'side', description: '侧边聊天' }], commands: [], skills: [] };
    else if (pathname.includes('/models')) body = { models: [], providers: [] };
    else if (pathname.includes('/experts')) body = { experts: [], items: [], capabilities: [] };
    else if (pathname.endsWith('/sessions')) body = { sessions: [] };
    else if (pathname.includes('/files')) body = { files: [], entries: [] };
    else if (pathname.includes('/git')) body = { is_repo: false, files: [], branches: [] };
    else if (pathname.endsWith('/ui-preferences')) body = { theme: 'light', color_theme: 'default', locale: 'zh-CN' };
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  await page.routeWebSocket('**/ws/**', ws => {
    ws.onMessage(data => {
      const message = JSON.parse(data);
      if (message.type === 'side_chat_start') {
        const payload = message.payload;
        sockets.push({ payload, send(type, extra) {
          ws.send(JSON.stringify({ type, payload: { request_id: payload.request_id, ...extra } }));
        } });
      }
    });
  });
  const base = `http://127.0.0.1:${server.httpServer.address().port}`;
  await page.goto(`${base}/side-chat-retention`);
  await page.locator('[contenteditable=true]').first().waitFor();
  const windowLocator = page.locator('[data-side-chat-window]');
  const input = page.getByRole('textbox', { name: '侧边聊天问题', exact: true });
  const open = async () => {
    await page.getByRole('button', { name: '会话菜单', exact: true }).click();
    await page.getByRole('menuitem', { name: '侧边聊天', exact: true }).click();
    await windowLocator.waitFor();
  };
  const switchTo = async id => {
    await page.evaluate(id => window.fixture.switch(id), id);
    await page.waitForFunction(id => document.querySelector('[data-fixture-session]')?.dataset.fixtureSession === id, id);
  };
  const advance = ms => page.evaluate(ms => window.fixture.advance(ms), ms);
  const settings = async value => {
    await page.evaluate(value => window.fixture.settings(value), value);
    if (value) await page.waitForFunction(() => {
      const mask = document.querySelector('[data-settings-mask]');
      const panel = document.querySelector('[data-settings-window]');
      return mask && panel && getComputedStyle(mask).opacity === '1' && getComputedStyle(panel).opacity === '1';
    });
  };
  async function until(fn) {
    for (let i = 0; i < 100; i++) {
      if (await fn()) return;
      await page.waitForTimeout(50);
    }
    throw new Error('side chat fixture condition timed out');
  }
  async function submit(question) {
    const count = sockets.length;
    await input.fill(question);
    await input.press('Enter');
    await until(() => sockets.length === count + 1);
    return sockets.at(-1);
  }
  await settings(true);
  const retention = page.getByRole('spinbutton', { name: '侧边聊天存续时间' });
  await retention.waitFor();
  assert.equal(await retention.inputValue(), '30');
  assert.equal(await retention.getAttribute('min'), '1');
  await retention.fill('2');
  await retention.press('Tab');
  await until(() => page.evaluate(() => localStorage.getItem('ace.sideChatRetentionMinutes') === '2'));
  await retention.fill('');
  await retention.press('Tab');
  assert.equal(await retention.inputValue(), '2');
  await retention.fill('0');
  await retention.press('Tab');
  assert.equal(await retention.inputValue(), '2');
  await settings(false);
  await settings(true);
  await retention.waitFor();
  assert.equal(await retention.inputValue(), '2');
  await page.screenshot({ path: path.join(output, 'settings-light-desktop.png') });
  await page.evaluate(() => window.fixture.theme('dark'));
  await page.waitForFunction(() => document.documentElement.dataset.theme === 'dark');
  await page.setViewportSize({ width: 720, height: 1000 });
  await page.screenshot({ path: path.join(output, 'settings-dark-narrow.png') });
  const row = retention.locator('xpath=../..');
  assert.equal(await row.evaluate(element => element.scrollWidth <= element.clientWidth), true);
  await settings(false);
  await page.setViewportSize({ width: 1440, height: 1000 });

  await open();
  const first = await submit('保留第一轮侧聊');
  first.send('side_chat_delta', { delta: '长回答内容' });
  await until(() => windowLocator.innerText().then(text => text.includes('长回答内容')));
  await advance(40 * 60_000);
  assert.equal(await input.isDisabled(), true);
  first.send('side_chat_done', { answer: '长回答内容', cancelled: false });
  await until(() => input.isEnabled());
  await input.fill('未发送的草稿');
  await switchTo('retention-b');
  await until(() => windowLocator.count().then(count => count === 0));
  await open();
  assert.equal(await windowLocator.locator('.ace-side-chat-turn').count(), 0);
  await advance(60_000);
  await switchTo('retention-a');
  await windowLocator.waitFor();
  assert.equal(await input.inputValue(), '未发送的草稿');
  assert.match(await windowLocator.innerText(), /长回答内容/);
  await page.evaluate(() => window.fixture.mount(false));
  await until(() => windowLocator.count().then(count => count === 0));
  await page.evaluate(() => window.fixture.mount(true));
  await windowLocator.waitFor();
  assert.equal(await input.inputValue(), '未发送的草稿');
  const followup = await submit('继续追问');
  assert.deepEqual(followup.payload.history, [
    { role: 'user', content: '保留第一轮侧聊' },
    { role: 'assistant', content: '长回答内容' },
  ]);
  followup.send('side_chat_done', { answer: '续聊回答', cancelled: false });
  await until(() => input.isEnabled());
  await advance(2 * 60_000);
  assert.equal(await windowLocator.locator('.ace-side-chat-turn').count(), 2, 'visible state survives expiry');
  await page.screenshot({ path: path.join(output, 'side-chat-retained.png') });
  await switchTo('retention-b');
  await switchTo('retention-a');
  await until(() => windowLocator.count().then(count => count === 0));
  await open();
  assert.equal(await windowLocator.locator('.ace-side-chat-turn').count(), 0);
  assert.equal(await input.inputValue(), '');
  const fresh = await submit('过期后的新聊天');
  assert.deepEqual(fresh.payload.history, []);
  fresh.send('side_chat_delta', { delta: '停止前的部分回答' });
  await until(() => windowLocator.innerText().then(text => text.includes('停止前的部分回答')));
  await switchTo('retention-b');
  await switchTo('retention-a');
  await until(() => input.isEnabled());
  assert.match(await windowLocator.innerText(), /停止前的部分回答/);
  assert.equal(await windowLocator.locator('[data-side-chat-status="stopped"]').count(), 1);
  await page.getByRole('button', { name: '最小化侧边聊天', exact: true }).click();
  await switchTo('retention-b');
  await switchTo('retention-a');
  assert.equal(await windowLocator.count(), 0);
  await settings(true);
  await retention.fill('1');
  await retention.press('Tab');
  await settings(false);
  await advance(60_000);
  await switchTo('retention-b');
  await switchTo('retention-a');
  await open();
  assert.equal(await windowLocator.locator('.ace-side-chat-turn').count(), 0, 'latest preference applies');
  await page.getByRole('button', { name: '最小化侧边聊天', exact: true }).click();
  await page.reload();
  await page.locator('[contenteditable=true]').first().waitFor();
  await settings(true);
  await retention.waitFor();
  assert.equal(await retention.inputValue(), '1', 'preference survives reload');
  await page.evaluate(() => window.fixture.locale('en-US'));
  const english = page.getByRole('spinbutton', { name: 'Side chat retention time' });
  await english.waitFor();
  assert.equal(await english.inputValue(), '1');
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ ok: true, coverage: 'real SettingsPage and ChatView; settings persistence/validation; session isolation, history/draft/open restore, remount/StrictMode, long generation, expiry on return, interrupted output, newest preference, English, light/dark/narrow', output }));
} finally {
  await browser?.close();
  await server.close();
}
