import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

// Run from web: node scripts/test-busy-reasoning.mjs [--baseline]
// Uses real ChatView with fixture HTTP/WebSocket traffic; no live daemon.
// ACE_PLAYWRIGHT_MODULE / ACE_CHROMIUM_EXECUTABLE select optional browser tools.
// ACE_REASONING_TEST_OUTPUT optionally saves desktop/narrow screenshots.
const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const baseline = process.argv.includes('--baseline')
  ? execFileSync('git', ['show', 'HEAD:web/src/components/ChatView.jsx'], { cwd: web, encoding: 'utf8' }) : null;
const entry = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {ChatView} from '/src/components/ChatView.jsx';
import {connection} from '/src/lib/connection.js';
import {applyLocalePreference} from '/src/i18n/index.js';
import '/src/styles/globals.css';
await applyLocalePreference('zh-CN');
const root = createRoot(document.getElementById('root'));
const empty = [];
window.fixture = {
  render(sid) {
    root.render(<ChatView sessionRef={{sessionId:sid,title:'Reasoning regression',workspaceHash:''}}
      health={{}} permissionRequests={empty} recentExpertIds={empty}/>);
  },
  busy(sid) {
    connection.dispatchEvent(new CustomEvent('message', {detail:{type:'busy_changed',session_id:sid,payload:{busy:true,turn_id:'active-turn'}}}));
  },
};
window.fixture.render('reasoning-a');
`;
const html = `<!doctype html><html><head><meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/></head><body>
<div id="root" style="height:100vh;display:flex;width:100%;background:var(--ace-bg);color:var(--ace-fg)"></div>
<script>
class FixtureWebSocket extends EventTarget {
  static OPEN=1; static CLOSED=3;
  constructor(){super();this.readyState=1;setTimeout(()=>this.dispatchEvent(new Event('open')),0);}
  send(){} close(){this.readyState=3;}
}
window.WebSocket=FixtureWebSocket;
</script><script type="module" src="/@id/virtual:reasoning-fixture"></script></body></html>`;
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'busy-reasoning-fixture', enforce: 'pre',
    resolveId(id) { if (id === 'virtual:reasoning-fixture') return '\0virtual:reasoning-fixture'; },
    async load(id) {
      if (id === '\0virtual:reasoning-fixture') return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx', jsx: 'automatic' })).code;
      if (baseline && id.replaceAll('\\', '/') === path.join(web, 'src/components/ChatView.jsx').replaceAll('\\', '/')) return baseline;
    },
    configureServer(vite) {
      vite.middlewares.use('/reasoning-fixture', async (req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/reasoning-fixture', html));
      });
    },
  }],
});
const reasoning = { supported: true, default_enabled: true, supported_efforts: ['low', 'high'], default_effort: 'high' };
const model = (effort) => ({ name: 'fixture', provider: 'openai', model: 'fixture-model',
  models_dev_provider_id: 'acemodel', reasoning: { ...reasoning, effort: effort ?? 'high' }, reasoning_effort: effort });
const states = { 'reasoning-a': model('high'), 'reasoning-b': model('low') };
const errors = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  page.setDefaultTimeout(60000);
  page.on('pageerror', error => errors.push(error.message));
  let onUpdate;
  await page.route('**/api/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    const sid = pathname.match(/\/sessions\/([^/]+)/)?.[1];
    let data = {};
    if (pathname.endsWith('/reasoning')) {
      const effort = route.request().postDataJSON().effort;
      const result = await new Promise(resolve => onUpdate({ sid, effort, reply: resolve }));
      if (result.status === 200) states[sid] = model(effort);
      await route.fulfill({ status: result.status, contentType: 'application/json',
        body: JSON.stringify(result.status === 200 ? states[sid] : { error: 'REASONING_UPDATE_FAILED' }) });
      return;
    }
    if (pathname.endsWith('/model')) data = states[sid];
    else if (pathname.includes('/messages')) data = { messages: [{ role: 'user', content: '正在运行的任务', uuid: 'active-turn' }], events: [], busy: true, status: 'running' };
    else if (pathname.includes('models')) data = { models: [model(null)], profiles: [model(null)] };
    else if (pathname.includes('workspaces')) data = { workspaces: [] };
    else if (pathname.includes('history')) data = { items: [], history: [] };
    else if (pathname.includes('commands')) data = { commands: [] };
    else if (pathname.includes('skills')) data = { skills: [] };
    else if (pathname.includes('experts')) data = { experts: [] };
    else if (pathname.includes('subagents')) data = { tasks: [] };
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(data) });
  });
  const selector = () => page.getByRole('button', { name: /^思考深度：/ });
  const choose = async label => {
    await selector().click();
    let timer;
    const request = new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new Error('reasoning update was not sent')), 10000);
      onUpdate = update => { clearTimeout(timer); resolve(update); };
    });
    // Attach the rejection handler before Playwright awaits the click.
    request.catch(() => {});
    await page.getByRole('menuitemradio', { name: label, exact: true }).click();
    const received = await request;
    assert.equal(await selector().isDisabled(), true, 'pending update prevents duplicate changes');
    return received;
  };
  const waitLabel = async label => {
    await page.getByRole('button', { name: `思考深度：${label}`, exact: true }).waitFor();
    await page.waitForFunction(() => !document.querySelector('[aria-label^="思考深度："]')?.disabled);
  };
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/reasoning-fixture`, { waitUntil: 'commit' });
  await selector().waitFor();
  await page.evaluate(() => window.fixture.busy('reasoning-a'));
  await page.waitForTimeout(100);
  assert.equal(await selector().isEnabled(), true, 'busy session keeps reasoning selector enabled');
  let request = await choose('低');
  assert.equal(request.effort, 'low');
  assert.equal(request.sid, 'reasoning-a');
  request.reply({ status: 200 });
  await waitLabel('低');
  console.log('[pass] busy session changes effort and protects pending submission');

  request = await choose('高');
  request.reply({ status: 500 });
  await waitLabel('低');
  console.log('[pass] failed update preserves selection');

  request = await choose('高');
  await page.evaluate(() => window.fixture.render('reasoning-b'));
  await waitLabel('低');
  request.reply({ status: 200 });
  await page.waitForTimeout(100);
  assert.equal(await selector().getAttribute('aria-label'), '思考深度：低');
  console.log('[pass] late response cannot overwrite another session');

  await page.evaluate(() => window.fixture.busy('reasoning-b'));
  request = await choose('默认');
  assert.equal(request.effort, null);
  request.reply({ status: 200 });
  await waitLabel('高');
  console.log('[pass] busy session restores saved default');
  for (const width of [1280, 420]) {
    await page.setViewportSize({ width, height: 800 });
    await selector().click();
    await page.getByRole('menuitemradio', { name: '低', exact: true }).waitFor();
    if (process.env.ACE_REASONING_TEST_OUTPUT) {
      await mkdir(process.env.ACE_REASONING_TEST_OUTPUT, { recursive: true });
      await page.screenshot({ path: path.join(process.env.ACE_REASONING_TEST_OUTPUT, `busy-reasoning-${width}.png`) });
    }
    await page.keyboard.press('Escape');
  }
  assert.deepEqual(errors, []);
  console.log('[pass] desktop and narrow layouts keep menu usable without runtime errors');
} catch (error) {
  if (errors.length) console.error(errors);
  throw error;
} finally {
  await browser?.close();
  await server.close();
}
