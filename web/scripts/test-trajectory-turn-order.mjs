import assert from 'node:assert/strict';
import { mkdir } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

// Exercise the real trajectory view with independent HTTP and stream delivery.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const entry = `
import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import {TrajectoryView} from '/src/components/trajectory/TrajectoryView.jsx';
import {createTranscriptState, reduceTranscriptEvent} from '/src/lib/sessionTranscript.js';
import '/src/styles/globals.css';
let records = [], streamSequence = 0;
const api = { async getTrajectory(_id, options) {
  const incoming = options.tail ? records : records.filter(r => r.sequence > options.after);
  const last = records.at(-1)?.sequence || 0;
  window.fixture.servedSequence = last;
  return { records: incoming, next_after:last, recorded_latest_sequence:last, legacy_total:1, legacy_next_after:1 };
}};
function App() {
  const [session, setSession] = useState(0);
  const [state, setState] = useState(createTranscriptState());
  window.fixture = {
    servedSequence: window.fixture?.servedSequence || 0,
    reset(next) { records=next; setState(createTranscriptState()); setSession(n=>n+1); },
    records(next) { records=next; },
    emit(type, payload) { setState(s=>reduceTranscriptEvent(s,{type,payload,seq:++streamSequence}).state); },
  };
  return <TrajectoryView api={api} sessionId={'fixture-'+session} busy={state.busy} livePartial={state.trajectoryPartial}/>;
}
createRoot(document.getElementById('root')).render(<App/>);
`;
const html = `<!doctype html><html><head><meta charset="UTF-8"/></head><body>
<div id="root" style="height:100vh;display:flex;flex-direction:column"></div>
<script type="module" src="/@id/virtual:trajectory-turn-order"></script></body></html>`;
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'trajectory-turn-order-fixture',
    resolveId(id) { if (id === 'virtual:trajectory-turn-order') return '\0virtual:trajectory-turn-order'; },
    async load(id) {
      if (id === '\0virtual:trajectory-turn-order') {
        return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx' })).code;
      }
    },
    configureServer(vite) {
      vite.middlewares.use('/trajectory-turn-order', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/trajectory-turn-order', html));
      });
    },
  }],
});
const records = [{ sequence: null, legacy_index: 0, type: 'legacy_context',
  payload: { role: 'user', is_meta: true, content: 'request context' } }];
for (let turn = 1; turn <= 7; turn += 1) {
  for (const [type, payload] of [
    ['message', { role: 'user', id: `user-${turn}`, content: `input ${turn}` }],
    ['turn_start', { turn_id: `user-${turn}`, user_message_id: `user-${turn}` }],
    ['model_step_start', { step_index: 1 }],
    ['model_request', { step_index: 1, messages: [{ role: 'system', content: `prompt ${turn}` }] }],
    ['model_response', { step_index: 1, content: `answer ${turn}` }],
    ['turn_end', { turn_id: `user-${turn}` }],
  ]) records.push({ sequence: records.length, timestamp_ms: 1000 + records.length, type, payload });
}

const errors = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/trajectory-turn-order`);
  await page.waitForFunction(() => !!window.fixture);
  const reset = async next => {
    await page.evaluate(next => window.fixture.reset(next), next);
    await page.waitForFunction(last => window.fixture.servedSequence === last, next.at(-1)?.sequence || 0);
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  };
  const emit = async (type, payload) => {
    await page.evaluate(({type,payload}) => window.fixture.emit(type,payload), {type,payload});
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(resolve)));
  };
  const rows = () => page.locator('tr[data-kind]').evaluateAll(elements => {
    let turn = 1;
    return elements.map(row => {
      const label = row.querySelector('[aria-label^="Turn "]')?.getAttribute('aria-label');
      if (label) turn = Number(label.slice(5));
      return { turn, kind: row.dataset.kind, text: row.getAttribute('aria-label'), requestOnly: !!row.dataset.requestOnly };
    });
  });
  const assertTurns = async count => {
    const labels = await page.locator('tr[data-kind] [aria-label^="Turn "]').evaluateAll(nodes => nodes.map(n => n.getAttribute('aria-label')));
    assert.deepEqual(labels, Array.from({ length: count }, (_, i) => `Turn ${i + 1}`));
    for (const row of await rows()) {
      if (row.kind === 'user') assert.ok(row.text.endsWith(`input ${row.turn}`), JSON.stringify(row));
    }
  };
  // Starting with context alone must not consume Turn 1.
  await reset(records.slice(0, 1));
  await emit('busy_changed', { busy: true, turn_id: 'user-1' });
  await emit('model_step_start', { step_index: 1 });
  await emit('token', { text: 'first live' });
  await assertTurns(1);

  // Restore six historical turns but leave the transcript's ordinal at zero.
  // Stable turn identity must keep the seventh stream at the end of the ledger.
  await reset(records.slice(0, 37));
  await assertTurns(6);
  await emit('busy_changed', { busy: true, turn_id: 'user-7' });
  await emit('model_step_start', { step_index: 1 });
  await emit('token', { text: 'seventh live' });
  await assertTurns(7);
  assert.ok((await rows()).at(-1).text.endsWith('seventh live'));
  for (const end of [38, 39, 41, 42]) {
    await page.evaluate(next => window.fixture.records(next), records.slice(0, end));
    await page.waitForFunction(last => window.fixture.servedSequence === last, end - 1);
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
    await assertTurns(7);
    const last = (await rows()).filter(row => !row.requestOnly).at(-1);
    assert.equal(last.turn, 7);
    assert.ok(last.text.endsWith(end === 42 ? 'answer 7' : 'seventh live'), JSON.stringify(last));
  }
  assert.equal((await rows()).filter(row => row.text.includes('seventh live')).length, 0);
  await emit('busy_changed', { busy: false, turn_id: 'user-7' });
  await assertTurns(7);

  const output = path.resolve(process.env.ACE_TRAJECTORY_EVIDENCE_DIR || path.join(os.tmpdir(), 'ace-trajectory-turn-order'));
  await mkdir(output, { recursive: true });
  const screenshot = path.join(output, 'trajectory-turn-order.png');
  await page.screenshot({ path: screenshot, fullPage: true });
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ result: 'passed', screenshot, scenarios: ['context-only first turn', 'seven ordered turns', 'stream before polling', 'input/start/request/response polling boundaries', 'completed partial deduplication'] }));
} finally {
  await browser?.close();
  await server.close();
}
