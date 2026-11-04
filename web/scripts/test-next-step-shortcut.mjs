import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

// Real ChatView, transcript projection and Slate composer; daemon calls are fixtures.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-next-step-'));
const fixture = `
import React,{useMemo,useState} from 'react';
import {createRoot} from 'react-dom/client';
import {ChatView} from '/src/components/ChatView.jsx';
import {NextStepCard} from '/src/components/NextStepCard.jsx';
import {Modal} from '/src/components/Modal.jsx';
import '/src/i18n/index.js';
import '/src/styles/globals.css';
function Fixture(){
 const [sid,setSid]=useState('first'),[modal,setModal]=useState(false),[sending,setSending]=useState(false);
 const empty=useMemo(()=>[],[]);
 window.fixture={select:setSid,modal:setModal,sending:setSending};
 return <div className="bg-bg text-fg" style={{height:'100vh',display:'flex'}}>
  {location.search.includes('card')
   ? <NextStepCard step={{ask:'Continue?',actions:['Recommended','Other']}} sending={sending} onReply={label=>{window.cardReplies=(window.cardReplies||[]).concat(label);}}/>
   : <ChatView sessionRef={{sessionId:sid}} health={{}} permissionRequests={empty} recentExpertIds={empty}/>}
  {modal&&<Modal onClose={()=>setModal(false)}><textarea aria-label="Dialog editor"/></Modal>}
 </div>;
}
createRoot(document.getElementById('root')).render(<React.StrictMode><Fixture/></React.StrictMode>);
`;
const virtual = path.join(web, '__next-step-shortcut.jsx').replaceAll('\\', '/');
const html = `<html><body><div id="root"></div><script type="module">
import RefreshRuntime from '/@react-refresh'; RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;
window.__vite_plugin_react_preamble_installed__=true;</script>
<script type="module" src="/__next-step-shortcut.jsx"></script></body></html>`;
const server = await createServer({ root: web, logLevel: 'error', cacheDir: path.join(output, 'vite-cache'),
  plugins: [{ name: 'next-step-fixture', enforce: 'pre',
    resolveId(id) { if (id === '/__next-step-shortcut.jsx') return virtual; },
    load(id) { if (id.replaceAll('\\', '/') === virtual) return fixture; },
    configureServer(vite) { vite.middlewares.use((req, res, next) => {
      if (req.url?.split('?')[0] !== '/__next-step-shortcut') return next();
      res.setHeader('Content-Type', 'text/html'); res.end(html);
    }); },
  }], server: { host: '127.0.0.1', port: 0, open: false, hmr: false } });
const sends = [], errors = [];
const label = '继续运行相关测试';
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1100, height: 760 } });
  page.setDefaultTimeout(15000);
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => {
    localStorage.setItem('ace.locale', 'zh-CN');
    window.__ACECODE_OS__ = 'windows';
    class FixtureWebSocket extends EventTarget {
      static OPEN = 1;
      constructor() { super(); this.readyState = 1; }
      send() {} close() { this.readyState = 3; }
    }
    window.WebSocket = FixtureWebSocket;
  });
  await page.route('**/api/**', async route => {
    const request = route.request(), pathname = new URL(request.url()).pathname;
    const sid = pathname.split('/')[3];
    let body = { enabled: false, items: [] };
    if (pathname.endsWith('/messages') && request.method() === 'GET') body = { busy: false, events: [], messages: [
      { id: sid + '-u', role: 'user', content: '检查修改并给出下一步建议。' },
      { id: sid + '-a', role: 'assistant', content: '代码修改已完成，可以继续运行相关测试。' },
      { id: sid + '-done', role: 'tool', tool: 'task_complete', content: 'Shown in the next-step box.',
        metadata: { wrap_up: { recap: '代码已更新。', box: true, ask: '接下来要运行相关测试吗？',
          actions: [{ label: sid === 'long' ? label.repeat(12) : label }, { label: '稍后处理' }] } } },
      { role: 'system', metadata: { transcript_only: true, turn_timing: {
        user_message_uuid: sid + '-u', started_at_ms: 100, completed_at_ms: 200, duration_ms: 100, status: 'completed' } } },
    ] };
    else if (pathname.endsWith('/draft') && request.method() === 'GET') body = { text: '保留这段草稿' };
    else if (pathname.endsWith('/messages') && request.method() === 'POST') {
      sends.push({ sid, ...request.postDataJSON() }); body = { ok: true };
    }
    else if (pathname.endsWith('/commands')) body = { commands: [], skills: [], builtins: [] };
    else if (pathname === '/api/models') body = [];
    else if (pathname.endsWith('/workspaces')) body = { workspaces: [] };
    else if (pathname.endsWith('/sessions')) body = { sessions: [] };
    else if (pathname.includes('/experts')) body = { experts: [], items: [], capabilities: [] };
    else if (pathname.includes('/files')) body = { files: [], entries: [] };
    else if (pathname.includes('/git')) body = { is_repo: false, files: [], branches: [] };
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  const url = `http://127.0.0.1:${server.httpServer.address().port}/__next-step-shortcut`;
  await page.goto(url);
  const primary = page.locator('[data-next-step-primary="true"]');
  const card = page.locator('[data-next-step-box]');
  const editor = page.locator('[data-main-composer="true"] [data-ace-rich-composer]');
  const settle = () => page.evaluate(() => new Promise(resolve => setTimeout(resolve, 80)));
  const select = async sid => {
    await page.evaluate(sid => window.fixture.select(sid), sid);
    await primary.waitFor(); await settle();
  };
  const keyEvent = extra => page.evaluate(extra => document.activeElement.dispatchEvent(new KeyboardEvent('keydown',
    { key: 'Enter', code: 'Enter', ctrlKey: true, bubbles: true, cancelable: true, ...extra })), extra);
  await primary.waitFor(); await editor.waitFor();
  assert.equal(await primary.getAttribute('aria-keyshortcuts'), 'Control+Enter');
  assert.equal(await card.locator('kbd').count(), 2);
  await editor.click();
  const draft = await editor.textContent();
  assert.match(draft, /保留这段草稿/);
  await page.keyboard.press('Control+Enter'); await settle();
  assert.equal(sends.length, 1); assert.equal(sends[0].text, label);
  assert.equal(await editor.textContent(), draft);
  assert.equal(await card.count(), 0);
  console.log('[pass] Ctrl+Enter sends only the primary reply and preserves the actual composer draft');

  await select('guards');
  await page.evaluate(() => { document.activeElement.blur(); });
  for (const extra of [{ ctrlKey: false }, { shiftKey: true }, { altKey: true }, { ctrlKey: false, metaKey: true },
    { isComposing: true }, { keyCode: 229 }, { repeat: true }]) await keyEvent(extra);
  assert.equal(sends.length, 1);
  await editor.click();
  await page.evaluate(() => document.activeElement.dispatchEvent(new CompositionEvent('compositionstart', { bubbles: true })));
  await keyEvent({});
  await page.evaluate(() => {
    const target = document.activeElement;
    target.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true }));
    target.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: 'Enter', ctrlKey: true, bubbles: true, cancelable: true }));
  });
  await settle(); assert.equal(sends.length, 1);
  await page.evaluate(() => window.fixture.modal(true));
  await page.getByRole('textbox', { name: 'Dialog editor' }).click();
  await page.keyboard.press('Control+Enter'); await settle(); assert.equal(sends.length, 1);
  await page.evaluate(() => window.fixture.modal(false));
  for (const kind of ['input', 'terminal', 'side-chat', 'preview', 'menu']) {
    await page.evaluate(kind => {
      const host = document.createElement('div'); host.id = 'competing';
      if (kind === 'terminal') host.className = 'xterm';
      if (kind === 'side-chat') host.setAttribute('data-side-chat-window', 'true');
      if (kind === 'preview') host.setAttribute('data-ace-editable-preview-text', 'true');
      if (kind === 'menu') host.setAttribute('role', 'menu');
      const input = document.createElement(kind === 'menu' ? 'button' : 'textarea');
      host.append(input); document.body.append(host); input.focus();
    }, kind);
    await page.keyboard.press('Control+Enter'); await settle(); assert.equal(sends.length, 1, kind);
    await page.evaluate(() => document.getElementById('competing').remove());
  }
  console.log('[pass] exact modifiers, repeat, IME settling, dialogs, menus, terminal, side chat and other editors do not confirm');

  // One batched responsive/theme inspection using the complete conversation UI.
  for (const [sid, width, theme] of [['visual', 1100, 'light'], ['long', 390, 'dark']]) {
    await select(sid); await page.setViewportSize({ width, height: 760 });
    await page.evaluate(theme => {
      document.documentElement.setAttribute('data-theme', theme);
      document.documentElement.classList.toggle('dark', theme === 'dark');
    }, theme);
    await primary.scrollIntoViewIfNeeded();
    const button = await primary.boundingBox(), hint = await primary.locator('kbd').last().boundingBox();
    assert.ok(hint.x + hint.width <= button.x + button.width);
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
    await page.screenshot({ path: path.join(output, theme + '.png') });
  }
  await select('dismiss'); await card.getByRole('button', { name: '关闭' }).click();
  await page.evaluate(() => document.activeElement.blur()); await keyEvent({});
  assert.equal(sends.length, 1);
  await select('secondary');
  await card.getByRole('button', { name: '稍后处理', exact: true }).click(); await settle();
  assert.equal(sends.length, 2); assert.equal(sends[1].text, '稍后处理');
  console.log('[pass] light desktop/dark narrow layout, long-label keycaps, dismissal cleanup and secondary pointer action');

  await page.goto(url + '?card'); await primary.waitFor();
  await page.evaluate(() => window.fixture.sending(true)); await settle();
  await keyEvent({}); assert.equal(await page.evaluate(() => (window.cardReplies || []).length), 0);
  await page.evaluate(() => window.fixture.sending(false)); await settle();
  await keyEvent({ repeat: true }); assert.equal(await page.evaluate(() => (window.cardReplies || []).length), 0);
  await keyEvent({}); assert.deepEqual(await page.evaluate(() => window.cardReplies), ['Recommended']);
  assert.deepEqual(errors, []);
  console.log('[pass] disabled sending and StrictMode listener cleanup; no browser errors');
  console.log(JSON.stringify({ artifacts: output }));
} finally { await browser?.close(); await server.close(); }
