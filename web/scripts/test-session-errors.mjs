import assert from 'node:assert/strict';
import { mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

// Real ChatView, with isolated session-history and event fixtures.
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
function Fixture() {
  const [sid, setSid] = React.useState('quota-session');
  window.fixture = {
    select: setSid,
    emit(payload, seq) { connection.dispatchEvent(new CustomEvent('message', {
      detail: {type:'message', payload, seq, session_id:'quota-session'}
    })); },
  };
  return <ChatView sessionRef={{sessionId:sid,title:sid,workspaceHash:''}}
    health={{}} permissionRequests={[]} recentExpertIds={[]}/>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const html = `<!doctype html><html><head><meta charset="UTF-8"/></head><body>
<div id="root" style="height:100vh;display:flex;width:100%;background:var(--ace-bg);color:var(--ace-fg)"></div>
<script>
class FixtureWebSocket extends EventTarget {
  static OPEN=1; static CLOSED=3;
  constructor(){super();this.readyState=1;setTimeout(()=>this.onopen?.({}),0);}
  send(){} close(){this.readyState=3;}
}
window.WebSocket=FixtureWebSocket;
</script><script type="module" src="/@id/virtual:session-errors"></script></body></html>`;
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'session-errors',
    resolveId(id) { if (id === 'virtual:session-errors') return '\0virtual:session-errors'; },
    async load(id) { if (id === '\0virtual:session-errors') return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx' })).code; },
    configureServer(vite) {
      vite.middlewares.use('/session-errors', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/session-errors', html));
      });
    },
  }],
});
const error = {
  id: 'persisted-error-451', uuid: 'persisted-error-451', role: 'error',
  content: '[Error] HTTP 451 额度已经用完', timestamp: '2026-10-10T07:00:00Z',
  metadata: { transcript_only: true, provider_error: { status_code: 451, raw_body: '额度已经用完' } },
};
const messages = [{ id: 'quota-user', role: 'user', content: '请继续处理任务' }];
let replay = [];
const errors = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1280, height: 850 } });
  page.setDefaultTimeout(15000);
  page.on('pageerror', error => errors.push(error.message));
  await page.route('**/api/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    let data = {};
    if (pathname.includes('/messages')) data = {
      messages: pathname.includes('quota-session') ? messages : [{ id: 'other-user', role: 'user', content: '另一个会话' }],
      events: pathname.includes('quota-session') ? replay : [], busy: false,
    };
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
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/session-errors`, { waitUntil: 'commit' });
  await page.getByText('请继续处理任务', { exact: true }).waitFor();
  await page.evaluate(() => window.fixture.select('other-session'));
  await page.getByText('另一个会话', { exact: true }).waitFor();
  messages.push(error); // Failure occurs while the other session is selected.
  await page.evaluate(() => window.fixture.select('quota-session'));
  const notice = page.getByText('HTTP 451 额度已经用完', { exact: false });
  await notice.waitFor();
  assert.equal(await notice.count(), 1);
  await page.evaluate(error => window.fixture.emit(error, 10), error);
  assert.equal(await notice.count(), 1);
  replay = [{ type: 'message', seq: 10, payload: error }];
  await page.reload({ waitUntil: 'commit' });
  await notice.waitFor();
  assert.equal(await notice.count(), 1);
  const second = { ...error, id: 'persisted-error-451-again', uuid: 'persisted-error-451-again' };
  messages.push({ id: 'quota-user-2', role: 'user', content: '再试一次' }, second);
  replay.push({ type: 'message', seq: 11, payload: second });
  await page.reload({ waitUntil: 'commit' });
  await page.waitForFunction(() => [...document.querySelectorAll('*')].filter(el =>
    el.children.length === 0 && el.textContent.includes('HTTP 451 额度已经用完')).length === 2);
  assert.equal(await notice.count(), 2);
  const out = path.resolve(web, '../build/validation/session-errors');
  await mkdir(out, { recursive: true });
  await page.screenshot({ path: path.join(out, 'restored-errors.png'), fullPage: true });
  assert.deepEqual(errors, []);
  console.log('[pass] background error survives switch and refresh; replay deduplicates; repeated failures remain visible');
} finally {
  await browser?.close();
  await server.close();
}
