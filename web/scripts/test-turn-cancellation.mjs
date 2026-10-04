import assert from 'node:assert/strict';
import { mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

// Real ChatView + controlled HTTP/WebSocket, without touching a live daemon.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const entry = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {ChatView} from '/src/components/ChatView.jsx';
import {connection} from '/src/lib/connection.js';
import {applyLocalePreference} from '/src/i18n/index.js';
import '/src/styles/globals.css';
await applyLocalePreference('zh-CN');
window.fixture = {
  emit(type, payload) { connection.dispatchEvent(new CustomEvent('message', {detail:{type,payload,session_id:'cancel-fixture'}})); },
  disconnect() { connection.ws.readyState=3; connection.dispatchEvent(new CustomEvent('disconnect')); },
  reconnect() { connection.ws.readyState=1; },
};
createRoot(document.getElementById('root')).render(<ChatView
  sessionRef={{sessionId:'cancel-fixture',title:'停止回归',workspaceHash:''}}
  health={{}} permissionRequests={[]} recentExpertIds={[]}/>);
`;
const html = `<!doctype html><html><head><meta charset="UTF-8"/></head><body>
<div id="root" style="height:100vh;display:flex;width:100%;background:var(--ace-bg);color:var(--ace-fg)"></div>
<script>
window.commands=[];
class FixtureWebSocket extends EventTarget {
  static OPEN=1; static CLOSED=3;
  constructor(){super();this.readyState=1;setTimeout(()=>this.onopen?.({}),0);}
  send(value){window.commands.push(JSON.parse(value));} close(){this.readyState=3;}
}
window.WebSocket=FixtureWebSocket;
</script><script type="module" src="/@id/virtual:cancel-fixture"></script></body></html>`;
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'cancel-fixture',
    resolveId(id) { if (id === 'virtual:cancel-fixture') return '\0virtual:cancel-fixture'; },
    async load(id) { if (id === '\0virtual:cancel-fixture') return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx' })).code; },
    configureServer(vite) {
      vite.middlewares.use('/cancel-fixture', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/cancel-fixture', html));
      });
    },
  }],
});
const messages = [{ id: 'u1', uuid: 'u1', role: 'user', content: '检查项目' }];
const submissions = [];
let busy = true, turnId = 't1';
const errors = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1280, height: 850 } });
  page.setDefaultTimeout(15000);
  page.on('pageerror', error => errors.push(error.message));
  await page.route('**/api/**', async route => {
    const request = route.request();
    const pathname = new URL(request.url()).pathname;
    let data = {};
    if (pathname.endsWith('/messages') && request.method() === 'POST') {
      submissions.push(request.postDataJSON());
      busy = true;
      data = { queued: true };
    } else if (pathname.includes('/messages')) data = { messages, events: [], busy, active_turn_id: turnId };
    else if (pathname.endsWith('/model')) data = { name: 'fixture', provider: 'openai', model: 'test' };
    else if (pathname.includes('models')) data = { models: [], profiles: [] };
    else if (pathname.includes('workspaces')) data = { workspaces: [] };
    else if (pathname.includes('history')) data = { items: [], history: [] };
    else if (pathname.includes('commands')) data = { commands: [] };
    else if (pathname.includes('skills')) data = { skills: [] };
    else if (pathname.includes('experts')) data = { experts: [] };
    else if (pathname.includes('subagents')) data = { tasks: [] };
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(data) });
  });
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/cancel-fixture`, { waitUntil: 'commit' });
  const emit = (type, payload) => page.evaluate(({ type, payload }) => window.fixture.emit(type, payload), { type, payload });
  await page.getByRole('button', { name: '中断', exact: true }).waitFor();
  for (const tool of ['vision_analyze', 'web_search', 'bash']) {
    await emit('tool_start', { tool, tool_call_id: tool, args: {} });
  }
  await page.getByRole('button', { name: '中断', exact: true }).click();
  await page.getByRole('button', { name: '正在停止…', exact: true }).waitFor();
  assert.equal(await page.getByRole('button', { name: '正在停止…', exact: true }).isDisabled(), true);
  assert.equal(await page.evaluate(() => window.commands.filter(c => c.type === 'abort').length), 1);
  await page.locator('[data-slate-editor="true"]').fill('停止后继续检查');
  await page.getByRole('button', { name: '排队', exact: true }).click();
  await page.getByText('停止后继续检查', { exact: true }).waitFor();
  assert.equal(submissions.length, 0, 'stop request cannot dispatch the next turn');
  for (const tool of ['vision_analyze', 'web_search', 'bash']) {
    await emit('tool_end', { tool, tool_call_id: tool, success: false, output: '[Interrupted]' });
    await emit('message', { role: 'tool_call', content: '[Tool: ' + tool + '] {}' });
    await emit('message', { role: 'tool_result', content: 'late result' });
  }
  const stop = { id: 'stop-t1', role: 'system', content: '[Interrupted]', metadata: { transcript_only: true, user_aborted: true, turn_id: 't1' } };
  messages.push(stop);
  await emit('message', stop);
  assert.equal(await page.getByText('用户已终止本轮任务', { exact: true }).count(), 1);
  busy = false;
  const accepted = page.waitForResponse(response => response.url().endsWith('/messages') && response.request().method() === 'POST');
  await emit('busy_changed', { busy: false, outcome: 'aborted', turn_id: 't1' });
  await page.waitForFunction(() => document.body.textContent.includes('停止后继续检查'));
  await accepted;
  assert.equal(submissions.length, 1);
  const submitted = submissions[0];
  assert.ok(submitted.client_message_id);
  await emit('done', { outcome: 'aborted', turn_id: 't1' });
  await page.getByRole('button', { name: '中断', exact: true }).waitFor();
  const canonical = { id: 'u2', role: 'user', content: submitted.text, metadata: { client_message_id: submitted.client_message_id } };
  messages.push(canonical);
  await emit('message', canonical);
  assert.equal(await page.getByText('停止后继续检查', { exact: true }).count(), 1);
  turnId = 't2';
  await emit('busy_changed', { busy: true, turn_id: 't2' });
  await page.evaluate(() => window.fixture.disconnect());
  await page.getByRole('button', { name: '中断', exact: true }).click();
  assert.equal(await page.getByRole('button', { name: '正在停止…', exact: true }).count(), 0);
  await page.evaluate(() => window.fixture.reconnect());
  await page.getByRole('button', { name: '中断', exact: true }).click();
  await page.getByRole('button', { name: '正在停止…', exact: true }).waitFor();
  if (process.env.ACE_CANCEL_TEST_OUTPUT) {
    await mkdir(process.env.ACE_CANCEL_TEST_OUTPUT, { recursive: true });
    await page.screenshot({ path: path.join(process.env.ACE_CANCEL_TEST_OUTPUT, 'stopping-turn.png') });
  }
  busy = false;
  turnId = '';
  await emit('done', { outcome: 'aborted', turn_id: 't2' });
  await page.locator('[data-slate-editor="true"]').fill('普通发送确认');
  const plainAccepted = page.waitForResponse(response => response.url().endsWith('/messages') && response.request().method() === 'POST');
  await page.getByRole('button', { name: '发送', exact: true }).click();
  await plainAccepted;
  await page.getByText('普通发送确认', { exact: true }).waitFor();
  assert.ok(submissions[1].client_message_id, 'ordinary sends carry correlation too');
  const ordinary = { id: 'u3', role: 'user', content: submissions[1].text, metadata: { client_message_id: submissions[1].client_message_id } };
  messages.push(ordinary);
  await emit('message', ordinary);
  assert.equal(await page.getByText('普通发送确认', { exact: true }).count(), 1);
  assert.deepEqual(errors, []);
  console.log('[pass] ChatView stop confirmation, visible queue, correlated input, stale done and disconnected retry');
} finally {
  await browser?.close();
  await server.close();
}
