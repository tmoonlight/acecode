import assert from 'node:assert/strict';
import { mkdir } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const entry = `
import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import {TrajectoryView} from '/src/components/trajectory/TrajectoryView.jsx';
import '/src/styles/globals.css';
let records=[], pageSize=10000;
const api={async getTrajectory(id, options={}) {
  const candidates=options.tail ? records.filter(r=>!options.before || r.sequence<options.before) : records.filter(r=>r.sequence>(options.after||0));
  const incoming=options.tail ? candidates.slice(-pageSize) : candidates;
  window.fixture.served=id;
  window.fixture.servedSequence=records.at(-1)?.sequence||0;
  return {records:incoming,first_sequence:incoming[0]?.sequence||0,recorded_has_older:options.tail&&candidates.length>incoming.length,
    next_after:incoming.at(-1)?.sequence||0,recorded_latest_sequence:records.at(-1)?.sequence||0};
}};
function App() {
  const [session,setSession]=useState(0);
  const [partial,setPartial]=useState(null);
  const [busy,setBusy]=useState(false);
  const [inspect,setInspect]=useState(null);
  window.fixture={served:window.fixture?.served,servedSequence:window.fixture?.servedSequence,
    reset(next,size=10000) {records=next;pageSize=size;setPartial(null);setBusy(false);setSession(n=>n+1);},
    stream(value) {setPartial(value);setBusy(true);},
    append(next) {records=next;setBusy(true);},
    inspect:setInspect,
  };
  return <TrajectoryView key={session} api={api} sessionId={'fixture-'+session} busy={busy}
    livePartial={partial} inspectCallId={inspect} onInspectApplied={()=>setInspect(null)}/>;
}
createRoot(document.getElementById('root')).render(<App/>);
`;
const html = '<!doctype html><html><head><meta charset="UTF-8"/></head><body><div id="root" style="height:100vh;display:flex;flex-direction:column"></div><script type="module" src="/@id/virtual:trajectory-inspector"></script></body></html>';
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'trajectory-inspector-fixture',
    resolveId(id) { if (id === 'virtual:trajectory-inspector') return '\0virtual:trajectory-inspector'; },
    async load(id) {
      if (id === '\0virtual:trajectory-inspector') return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx' })).code;
    },
    configureServer(vite) {
      vite.middlewares.use('/trajectory-inspector', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/trajectory-inspector', html));
      });
    },
  }],
});
const event = (sequence, type, payload = {}) => ({ sequence, type, payload, timestamp_ms: 1000 + sequence * 100 });
const code = 'print("hello")\nprint("world")\n';
const args = `{ "description": "inspect program", "code": ${JSON.stringify(code)} }`;
const long = 'line with a very long value '.repeat(200);
const png = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=';
const parts = [
  { type: 'text', text: 'mixed attachments' },
  { type: 'image', attachment: { id: 'i1', name: 'photo.png', mime_type: 'image/png', bytes: 68, width: 1, height: 1, url: png } },
  { type: 'file', attachment: { id: 'f1', name: `${'very-long-'.repeat(30)}empty.txt`, size: 0 } },
];
parts.push(parts[1]);
const sample = [
  event(1, 'message', { role: 'user', id: 'u1', content_parts: parts }),
  event(2, 'turn_start', { turn_id: 'u1', user_message_id: 'u1' }),
  event(3, 'model_step_start', { step_index: 1 }),
  event(4, 'model_request', { step_index: 1, messages: [{ role: 'system', content: 'system prompt' }], tools: [{ name: 'run_code', parameters: { properties: { code: { type: 'string', description: 'Python program' } } } }] }),
  event(5, 'model_response', { step_index: 1, status: 'retry', error: { display_message: 'temporary offline' } }),
  event(6, 'agent_progress', { phase: 'model_retry', retry_attempt: 1, retry_max_attempts: 3 }),
  event(7, 'model_first_output', { step_index: 1 }),
  event(8, 'model_response', { step_index: 1, status: 'completed', content: 'answer', reasoning_content: '# Reasoning\n\n**Check** the input.', tool_calls: [{ id: 'c1', name: 'run_code', arguments: args }] }),
  event(9, 'tool_start', { step_index: 1, tool: 'run_code', tool_call_id: 'c1', args }),
  event(10, 'tool_end', { step_index: 1, tool: 'run_code', tool_call_id: 'c1', success: true, output: JSON.stringify({ first: long, second: long }) }),
  event(11, 'model_step_finish', { step_index: 1, reason: 'completed' }),
  event(12, 'turn_end', { turn_id: 'u1' }),
];
let browser;
const errors = [];
const results = [];
const outputDir = path.join(os.tmpdir(), 'ace-trajectory-inspector');
try {
  await mkdir(outputDir, { recursive: true });
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1440, height: 960 } });
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => {
    window.copiedText = '';
    Object.defineProperty(navigator, 'clipboard', { value: { writeText: async text => { window.copiedText = text; } } });
  });
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/trajectory-inspector`);
  await page.waitForFunction(() => !!window.fixture);
  const frames = () => page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  const reset = async (records, size = 10000) => {
    await page.evaluate(({ records, size }) => { window.fixture.served = 'pending'; window.fixture.reset(records, size); }, { records, size });
    await page.waitForFunction(() => window.fixture.served !== 'pending' && document.querySelector('tr[data-kind]') && !document.querySelector('[aria-label="Loading trajectory…"]'));
    await frames();
  };
  const aside = page.locator('aside[aria-label="Event details"]');
  const tab = name => aside.getByRole('tab', { name, exact: true });
  await reset(sample);
  await page.evaluate(() => window.fixture.inspect('c1'));
  await tab('Code').click();
  await aside.getByRole('button', { name: 'Copy code', exact: true }).click();
  assert.equal(await page.evaluate(() => window.copiedText), code);
  assert.equal(await aside.locator('pre .line').count(), 2);
  await aside.getByRole('button', { name: 'Original JSON', exact: true }).click();
  await aside.getByRole('button', { name: 'Copy JSON', exact: true }).click();
  assert.equal(await page.evaluate(() => window.copiedText), args);
  assert.match(await aside.textContent(), /Turn 1.*Step 1/s);
  results.push('code identity, numbered source, exact source and raw JSON copying');

  await tab('Result').click();
  const expand = aside.getByRole('button', { name: 'Expand JSON node', exact: true });
  await expand.first().click();
  await expand.first().click();
  let wraps = aside.getByRole('button', { name: 'Wrap lines', exact: true });
  assert.equal(await wraps.count(), 2);
  await wraps.first().click();
  assert.equal(await wraps.nth(0).getAttribute('aria-pressed'), 'true');
  assert.equal(await wraps.nth(1).getAttribute('aria-pressed'), 'false');
  await aside.getByRole('button', { name: 'Collapse JSON node', exact: true }).nth(1).click();
  await expand.first().click();
  assert.equal(await wraps.nth(1).getAttribute('aria-pressed'), 'true');
  assert.equal(await page.evaluate(() => localStorage.getItem('ace.trajectory.jsonStringWrapping')), 'true');
  results.push('JSON string expansion, independent wrapping and persisted default');
  await page.screenshot({ path: path.join(outputDir, 'json-inspector.png'), animations: 'disabled' });

  await page.locator('tr[data-kind="message"]').filter({ hasText: 'answer' }).click();
  const thinking = aside.getByRole('button', { name: 'Thinking', exact: true });
  assert.equal(await thinking.getAttribute('aria-expanded'), 'true');
  assert.equal(await aside.locator('[data-markdown-variant="compact"] strong').textContent(), 'Check');
  const heading = aside.locator('[data-markdown-variant="compact"] h1');
  assert.equal(await heading.evaluate(el => getComputedStyle(el).fontSize), '13px');
  assert.equal(await heading.evaluate(el => getComputedStyle(el).lineHeight), '20px');
  await page.locator('button[aria-label="Request #1"]').first().click();
  assert.match(await aside.textContent(), /Completed/);
  assert.match(await aside.textContent(), /Last attempt error.*temporary offline/s);
  results.push('compact thinking Markdown and recovered request status');

  await page.locator('tr[data-kind="user"]').click();
  assert.equal(await aside.locator('ul[aria-label="Attachments"] > li').count(), 3);
  assert.match(await aside.textContent(), /0 B/);
  assert.equal(await aside.locator('ul[aria-label="Attachments"]').evaluate(el => el.scrollWidth <= el.clientWidth + 1), true);
  await tab('Raw').click();
  assert.equal(await aside.locator('details').count(), 3);
  assert.equal(await aside.locator('details[open]').count(), 0);
  await aside.locator('details').nth(1).locator('summary').click();
  assert.match(await aside.locator('details').nth(1).textContent(), /"size": 0/);
  results.push('ordered repeated attachments, zero-byte file, bounded names and raw metadata');
  await tab('Summary').click();
  await page.screenshot({ path: path.join(outputDir, 'attachments-light.png'), animations: 'disabled' });
  await page.evaluate(() => {
    document.body.setAttribute('data-ds-dark-theme', '');
    document.documentElement.setAttribute('data-theme', 'dark');
  });
  await page.setViewportSize({ width: 780, height: 800 });
  await page.waitForFunction(() => getComputedStyle(document.querySelector('aside')).backgroundColor === 'rgb(35, 35, 36)');
  await page.screenshot({ path: path.join(outputDir, 'attachments-dark-narrow.png'), animations: 'disabled' });
  await page.evaluate(() => {
    document.body.removeAttribute('data-ds-dark-theme');
    document.documentElement.removeAttribute('data-theme');
  });
  await page.setViewportSize({ width: 1440, height: 960 });

  await reset(sample.slice(0, 7));
  await page.evaluate(() => window.fixture.stream({ turnId: 'u1', step: 1, blocks: [{ kind: 'text', text: 'streaming' }] }));
  await page.locator('tr[data-kind="message"]').filter({ hasText: 'streaming' }).click();
  assert.match(await aside.textContent(), /Pending/);
  assert.doesNotMatch(await aside.textContent(), /Completed/);
  results.push('live assistant stays pending');

  const history = [];
  for (let turn = 1; turn <= 80; turn += 1) {
    for (const [type, payload] of [
      ['message', { role: 'user', id: `h${turn}`, content: `input ${turn}` }],
      ['turn_start', { turn_id: `h${turn}`, user_message_id: `h${turn}` }],
      ['model_step_start', { step_index: 1 }],
      ['model_request', { step_index: 1, messages: [], tools: [] }],
      ['model_response', { step_index: 1, status: 'completed', content: `answer ${turn}` }],
      ['turn_end', { turn_id: `h${turn}` }],
    ]) history.push(event(history.length + 1, type, payload));
  }
  await reset(history, 240);
  const scroller = page.locator('[data-trajectory-scroll]');
  await scroller.evaluate(el => { el.scrollTop = 0; el.dispatchEvent(new Event('scroll')); });
  await scroller.getByRole('button', { name: 'Load earlier history', exact: true }).click();
  if (await page.locator('[data-virtual-position]').count() === 0) {
    await scroller.getByRole('button', { name: 'Load earlier history', exact: true }).click();
  }
  await page.waitForFunction(() => document.querySelector('[data-virtual-position]'));
  assert.ok(await page.locator('tr[data-kind]').count() < 100);
  await page.getByRole('searchbox', { name: 'Search trajectory' }).fill('answer 80');
  await page.waitForFunction(() => document.querySelectorAll('tr[data-kind]').length <= 2);
  assert.match(await page.locator('tr[data-kind]').last().textContent(), /answer 80/);
  results.push('history prepend, virtualization and search');
  await page.getByRole('searchbox', { name: 'Search trajectory' }).fill('');
  await frames();
  await scroller.evaluate(el => { el.scrollTop = 400; el.dispatchEvent(new Event('scroll')); });
  await frames();
  const beforeAppend = await scroller.evaluate(el => el.scrollTop);
  const appended = [...history, event(481, 'message', { role: 'user', id: 'h81', content: 'latest appended' })];
  await page.evaluate(next => window.fixture.append(next), appended);
  await page.waitForFunction(() => window.fixture.servedSequence === 481);
  await frames();
  assert.ok(Math.abs(await scroller.evaluate(el => el.scrollTop) - beforeAppend) <= 1);
  results.push('new records preserve the scrolled history position');

  await reset([...sample, ...history.map(record => ({ ...record, sequence: record.sequence + 12 }))]);
  assert.equal(await page.locator('tr[data-kind="tool"]').count(), 0);
  await page.evaluate(() => window.fixture.inspect('c1'));
  await tab('Code').click();
  assert.equal(await aside.locator('pre .line').count(), 2);
  results.push('cross-view tool inspection reveals older resident nodes');
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ result: 'passed', scenarios: results, screenshots: outputDir }));
} finally {
  await browser?.close();
  await server.close();
}
