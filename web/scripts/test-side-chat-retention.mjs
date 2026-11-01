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
const empty = [];
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
      {mounted && <ChatView sessionRef={{sessionId:sid,workspaceHash:'retention-qa'}} health={{}}
        permissionRequests={empty} recentExpertIds={empty} onOpenModelSettings={()=>setSettings(true)}/>}
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
const uploads = [];
const modelChanges = [];
const models = [{name:'vision-a',provider:'openai',model:'vision-a'}, {name:'text-b',provider:'openai',model:'text-b', reasoning:{supported:true, default_enabled:true, supported_efforts:['low','high'], default_effort:'high'}}];
const sockets = [];
let browser;
let page;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  page = await browser.newPage({ viewport: { width: 1440, height: 1000 }, locale: 'zh-CN' });
  page.on('pageerror', error => errors.push(error.stack || error.message));
  page.on('console', message => {
    if (message.type() === 'error' && message.text().includes('The above error occurred')) console.error(message.text());
  });
  await page.addInitScript(() => {
    // Playwright's WebSocket mock uses URL.parse, added after Chromium 110.
    // Keep that test transport usable on the older Desktop WebView baseline.
    if (!URL.parse) URL.parse = (input, base) => {
      try { return new URL(input, base); } catch { return null; }
    };
    window.sideChatNow = Date.now();
    Date.now = () => window.sideChatNow;
    window.__ACECODE_DESKTOP_SHELL__ = true;
    window.__ACECODE_OS__ = 'windows';
    window.aceDesktop_agentBrowserGetState = async () => ({ pages: [] });
    window.aceDesktop_agentBrowserSetLayout = async () => ({ ok: true });
    window.aceDesktop_agentBrowserCreatePage = async () => ({ ok: true });
  });
  await page.route('**/api/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body = {};
    if (pathname.endsWith('/attachments') && route.request().method() === 'POST') {
      const payload = route.request().postDataJSON(); uploads.push(payload);
      body = {attachment:{id:'attachment-'+uploads.length, name:payload.name, mime_type:payload.mime_type, kind:payload.mime_type.startsWith('image/')?'image':'file', session_id:'retention-a'}};
    } else if (pathname === '/api/models') body = models;
    else if (pathname.endsWith('/model')) {
      if(route.request().method() !== 'GET') modelChanges.push(route.request().postDataJSON());
      body = models[0];
    } else if (pathname.endsWith('/messages')) body = { busy: false, messages: [
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
        sockets.push({ payload, stops: 0, send(type, extra) {
          ws.send(JSON.stringify({ type, payload: { request_id: payload.request_id, ...extra } }));
        } });
      } else if (message.type === 'side_chat_stop') {
        const request = sockets.find(item => item.payload.request_id === message.payload.request_id);
        if (request) request.stops++;
      }
    });
  });
  const base = `http://127.0.0.1:${server.httpServer.address().port}`;
  await page.goto(`${base}/side-chat-retention`);
  await page.locator('[contenteditable=true]').first().waitFor();
  const windowLocator = page.locator('[data-side-chat-window]');
  const input = page.getByRole('textbox', { name: '侧边聊天问题', exact: true });
  const toggle = page.getByRole('button', { name: '侧边聊天', exact: true });
  const draftText = () => input.evaluate(el => [...el.querySelectorAll('[data-slate-string]')].map(node => node.textContent).join('\n'));
  const open = async () => {
    await toggle.click();
    await windowLocator.waitFor();
  };
  const switchTo = async id => {
    await page.evaluate(id => window.fixture.switch(id), id);
    await page.waitForFunction(id => document.querySelector('[data-fixture-session]')?.dataset.fixtureSession === id, id, { timeout: 5000 });
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
    // fill mutates the contenteditable before Slate publishes its new draft.
    // Wait for the actual submit state, especially after an expired reset.
    await until(() => windowLocator.locator('[data-composer-action]').isEnabled());
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

  // Exercise the complete ChatView toolbar and composer, with an isolated transport.
  assert.equal(await toggle.getAttribute('aria-pressed'), 'false');
  assert.equal(await toggle.evaluate(el => el.nextElementSibling?.getAttribute('aria-label')), '打开浏览器');
  await open();
  assert.equal(await toggle.getAttribute('aria-pressed'), 'true');
  await input.fill('第一行');
  const beforeComposition = sockets.length;
  await input.dispatchEvent('keydown', { key: 'Enter', code: 'Enter', isComposing: true });
  assert.equal(sockets.length, beforeComposition);
  await input.press('Shift+Enter');
  await input.pressSequentially('second line');
  assert.equal(await draftText(), '第一行\nsecond line');
  const baseHeight = await input.evaluate(el => el.clientHeight);
  assert.ok(baseHeight >= 40, 'shared multiline base height');
  await toggle.click();
  await until(() => windowLocator.count().then(count => count === 0));
  await open();
  assert.equal(await draftText(), '第一行\nsecond line');
  await input.fill(Array.from({ length: 40 }, (_, i) => `line ${i}`).join('\n'));
  assert.equal(await input.evaluate(el => el.scrollHeight > el.clientHeight && el.clientHeight > 60), true);
  await input.fill('');
  const action = windowLocator.locator('[data-composer-action]');
  const buttonStyle = el => {
    const s = getComputedStyle(el);
    return [s.width, s.height, s.borderRadius, s.backgroundColor, s.color, el.querySelector('[data-icon-name]')?.dataset.iconName];
  };
  assert.deepEqual(await action.evaluate(buttonStyle), await page.locator('[data-fixture-session] [data-composer-action]').evaluate(buttonStyle));
  const started = await submit('side action test');
  assert.equal(await action.getAttribute('data-composer-action'), 'stop');
  assert.equal(await input.evaluate(el => document.activeElement === el || el.contains(document.activeElement)), true);
  assert.equal(await action.evaluate(el => el.matches(':focus-visible')), false);
  await input.fill('queued follow-up');
  assert.equal(await action.getAttribute('data-composer-action'), 'queue');
  await action.click();
  await windowLocator.getByRole('list', { name: '排队中的待发送消息' }).waitFor();
  assert.equal(await draftText(), '');
  const requestCount = sockets.length;
  started.send('side_chat_done', { answer: 'first completed' });
  await until(() => sockets.length === requestCount + 1);
  const queuedRequest = sockets.at(-1);
  assert.equal(queuedRequest.payload.question, 'queued follow-up');
  assert.equal(queuedRequest.payload.history.length, 2);
  await input.fill('paused follow-up');
  await input.press('Enter');
  await action.click();
  await until(() => queuedRequest.stops === 1);
  assert.equal(await action.isDisabled(), true);
  queuedRequest.send('side_chat_done', { answer: 'partial', cancelled: true });
  await until(() => action.getAttribute('data-composer-action').then(mode => mode === 'resume'));
  await windowLocator.getByRole('button', { name: '取消排队' }).click();
  assert.equal(await action.getAttribute('data-composer-action'), 'send');
  await windowLocator.getByRole('button', { name: '清空侧边聊天' }).click();
  assert.equal(await windowLocator.locator('[data-composer-control="permission"]').count(), 0);
  assert.equal(await windowLocator.locator('[data-composer-control="add-context"]').count(), 0);
  assert.equal(await page.locator('[data-fixture-session] [data-composer-control="permission"]').count(), 1);
  // Both composers run the real InputBar; drop into the float must not reach the main draft.
  const mainDraftBefore = await page.locator('[data-fixture-session] [contenteditable=true]').innerText();
  await input.evaluate(el => {
    // Exercise browser File drops here; native path materialization is covered
    // by the shared composer's dedicated desktop drop fixture.
    window.__ACECODE_DESKTOP_SHELL__ = false;
    const data = new DataTransfer();
    const png = Uint8Array.from(atob('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII='), char => char.charCodeAt(0));
    data.items.add(new File([png], 'pixel.png', { type: 'image/png' }));
    data.items.add(new File(['file contents'], 'notes.txt', { type: 'text/plain' }));
    for (const type of ['dragenter', 'dragover', 'drop']) el.dispatchEvent(new DragEvent(type, { bubbles: true, cancelable: true, dataTransfer: data }));
  });
  await until(() => uploads.length === 2);
  await page.evaluate(() => { window.__ACECODE_DESKTOP_SHELL__ = true; });
  await until(() => action.isEnabled());
  await input.evaluate(el => window.dispatchEvent(new CustomEvent('acecode:session-reference-drop', { detail: {
    target: el, references: [{ session_id: 'other-session', workspace_hash: 'retention-qa', title: '参考会话', no_workspace: false }],
  } })));
  await until(() => input.innerText().then(text => text.includes('参考会话')));
  await windowLocator.locator('[data-composer-control="model"] button').click();
  await windowLocator.getByRole('option', { name: /text-b/ }).click();
  await windowLocator.getByRole('button', { name: /思考深度/ }).click();
  const reasoningMenu = page.getByRole('menu', { name: '思考深度', exact: true });
  await reasoningMenu.waitFor();
  assert.equal(await reasoningMenu.evaluate(el => +getComputedStyle(el).zIndex > +getComputedStyle(document.querySelector('[data-side-chat-window]')).zIndex), true);
  await reasoningMenu.getByRole('menuitemradio', { name: '低', exact: true }).click();
  await windowLocator.locator('[data-composer-control="model"] button').click();
  await windowLocator.locator('.ace-composer-model-settings').click();
  await page.locator('[data-settings-window]').waitFor();
  assert.equal(await windowLocator.count(), 0);
  await settings(false);
  await open();
  await toggle.click();
  await open();
  assert.match(await windowLocator.locator('[data-composer-control="model"]').innerText(), /text-b/);
  assert.equal(await windowLocator.locator('[data-composer-image-preview]').count(), 1);
  assert.match(await input.innerText(), /notes.txt/);
  assert.match(await input.innerText(), /参考会话/);
  await input.evaluate(el => {
    const clipboardData = new DataTransfer();
    clipboardData.setData('text/plain', 'pasted context '.repeat(180));
    el.dispatchEvent(new ClipboardEvent('paste', { bubbles: true, cancelable: true, clipboardData }));
  });
  const pasteCard = windowLocator.locator('[data-composer-pasted-text]');
  await pasteCard.waitFor();
  await pasteCard.click();
  const pasteDialog = page.getByRole('dialog').filter({ has: page.locator('[data-pasted-text-dialog]') });
  await pasteDialog.waitFor();
  assert.equal(await pasteDialog.evaluate(el => +getComputedStyle(el.parentElement).zIndex > +getComputedStyle(document.querySelector('[data-side-chat-window]')).zIndex), true);
  // The uncontrolled textarea receives its initial content in the next frame.
  await until(async () => (await pasteDialog.getByRole('textbox').inputValue()).startsWith('pasted context '));
  await pasteDialog.getByRole('textbox').fill('edited pasted context');
  await pasteDialog.getByRole('button', { name: '保存', exact: true }).click();
  await pasteDialog.waitFor({ state: 'hidden' });
  await windowLocator.locator('[data-composer-image-preview]').click();
  const preview = page.getByRole('dialog', { name: '图片预览', exact: true });
  await preview.waitFor();
  assert.equal(await preview.evaluate(el => +getComputedStyle(el).zIndex > +getComputedStyle(document.querySelector('[data-side-chat-window]')).zIndex), true);
  await preview.getByRole('button', { name: '关闭预览' }).click();
  const richCount = sockets.length;
  await action.click();
  await until(() => sockets.length === richCount + 1);
  const rich = sockets.at(-1);
  assert.equal(rich.payload.model_name, 'text-b');
  assert.equal(rich.payload.reasoning_effort, 'low');
  assert.equal(rich.payload.input.attachments.length, 2);
  assert.equal(rich.payload.input.session_references[0].session_id, 'other-session');
  assert.match(rich.payload.input.text, /edited pasted context/);
  assert.equal(await page.locator('[data-fixture-session] [contenteditable=true]').innerText(), mainDraftBefore);
  assert.deepEqual(modelChanges, []);
  rich.send('side_chat_done', { answer: 'rich response' });
  await until(() => windowLocator.locator('[data-side-chat-status="success"]').count().then(count => count === 1));
  const richFollowup = await submit('follow up');
  assert.equal(richFollowup.payload.history[0].input.attachments.length, 2);
  richFollowup.send('side_chat_done', { answer: 'follow-up response' });
  await until(() => windowLocator.locator('[data-side-chat-status="success"]').count().then(count => count === 2));
  await windowLocator.getByRole('button', { name: '清空侧边聊天' }).click();
  for (const theme of ['light', 'dark']) for (const width of [1440, 390]) {
    await page.evaluate(theme => window.fixture.theme(theme), theme);
    await page.setViewportSize({ width, height: 800 });
    await toggle.click();
    await open();
    await input.fill('多行输入\n第二行\n第三行');
    assert.equal(await toggle.evaluate(el => {
      const r = el.getBoundingClientRect();
      return el.contains(document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2));
    }), true, 'toggle is not covered by the float');
    assert.equal(await windowLocator.evaluate(el => {
      const r = el.getBoundingClientRect();
      return r.left >= 0 && r.right <= innerWidth && r.bottom <= innerHeight;
    }), true);
    await page.screenshot({ path: path.join(output, `composer-${theme}-${width}.png`) });
  }
  await windowLocator.getByRole('button', { name: '清空侧边聊天' }).click();
  await toggle.click();
  await page.setViewportSize({ width: 1440, height: 1000 });

  await open();
  const first = await submit('保留第一轮侧聊');
  first.send('side_chat_delta', { delta: '长回答内容' });
  await until(() => windowLocator.innerText().then(text => text.includes('长回答内容')));
  await advance(40 * 60_000);
  assert.equal(await input.isEnabled(), true);
  first.send('side_chat_done', { answer: '长回答内容', cancelled: false });
  await until(() => windowLocator.locator('[data-side-chat-status="success"]').count().then(count => count === 1));
  await input.fill('未发送的草稿');
  await switchTo('retention-b');
  await until(() => windowLocator.count().then(count => count === 0));
  await open();
  assert.equal(await windowLocator.locator('.ace-side-chat-turn').count(), 0);
  await advance(60_000);
  await switchTo('retention-a');
  await windowLocator.waitFor();
  assert.equal(await draftText(), '未发送的草稿');
  assert.match(await windowLocator.innerText(), /长回答内容/);
  await page.evaluate(() => window.fixture.mount(false));
  await until(() => windowLocator.count().then(count => count === 0));
  await page.evaluate(() => window.fixture.mount(true));
  await windowLocator.waitFor();
  assert.equal(await draftText(), '未发送的草稿');
  const followup = await submit('继续追问');
  assert.deepEqual(followup.payload.history.map(({role,content}) => ({role,content})), [
    { role: 'user', content: '保留第一轮侧聊' },
    { role: 'assistant', content: '长回答内容' },
  ]);
  followup.send('side_chat_done', { answer: '续聊回答', cancelled: false });
  await until(() => windowLocator.locator('[data-side-chat-status="success"]').count().then(count => count === 2));
  await advance(2 * 60_000);
  assert.equal(await windowLocator.locator('.ace-side-chat-turn').count(), 2, 'visible state survives expiry');
  await page.screenshot({ path: path.join(output, 'side-chat-retained.png') });
  await switchTo('retention-b');
  await switchTo('retention-a');
  await until(() => windowLocator.count().then(count => count === 0));
  await open();
  assert.equal(await windowLocator.locator('.ace-side-chat-turn').count(), 0);
  assert.equal(await draftText(), '');
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
  console.log(JSON.stringify({ ok: true, coverage: 'real SettingsPage and ChatView; shared rich InputBar, file/image/session drops, independent model/reasoning, rich history, image preview/menu layers; toolbar, multiline/IME, send/queue/stop/resume; session isolation, rich drafts, retention, English, light/dark/narrow', output }));
} catch (error) {
  console.error(JSON.stringify({ errors, output, session: await page?.evaluate(() => document.querySelector('[data-fixture-session]')?.dataset.fixtureSession) }));
  await page?.screenshot({ path: path.join(output, 'failure.png') }).catch(() => {});
  throw error;
} finally {
  await browser?.close();
  await server.close();
}
