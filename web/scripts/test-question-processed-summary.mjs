import assert from 'node:assert/strict';
import { mkdtemp } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer, transformWithEsbuild } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await mkdtemp(path.join(os.tmpdir(), 'ace-question-summary-'));
const entry = `
import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import {TranscriptItems} from '/src/components/TranscriptItems.jsx';
import {projectCollapsedTranscriptItems} from '/src/lib/transcriptProjection.js';
import {createTranscriptState, loadTranscriptHistory} from '/src/lib/sessionTranscript.js';
import '/src/styles/globals.css';
const messages = [
  {id:'user', role:'user', content:'清除用户数据排查邮件正文不显示问题', ts:1000},
  {id:'inspect', role:'assistant', content:'先确认清理范围', ts:2000},
  {id:'question-1', role:'tool', tool:'AskUserQuestion', content:'answered', ts:3000,
    metadata:{tool_success:true, ask_user_question_result:{items:[
      {question:'你想清除哪些范围的用户数据？', answer:'只清除 SDK 数据'}
    ]}}},
  {id:'check', role:'assistant', content:'检查应用状态', ts:4000},
  {id:'question-2', role:'tool', tool:'AskUserQuestion', content:'answered', ts:5000,
    metadata:{tool_success:true, ask_user_question_result:{items:[
      {question:'开发环境正在运行，请先关闭应用。', answer:'关了'}
    ]}}},
  {id:'cleanup', role:'assistant', content:'执行清理并检查结果', ts:6000},
  {id:'final', role:'assistant', content:'SDK 数据已清除完毕。', ts:7000},
  {id:'timing', role:'system', content:'[Turn timing]', metadata:{transcript_only:true,
    turn_timing:{user_message_uuid:'user', started_at_ms:1000,
      completed_at_ms:54028000, duration_ms:54027000, status:'completed'}}},
];
function App() {
  const [expanded, setExpanded] = useState(new Set());
  const [mode, setMode] = useState('final');
  window.fixture = {mode(value) {setExpanded(new Set()); setMode(value);}};
  let source = messages;
  if (mode === 'completion') source = messages.filter(m => m.id !== 'final' && m.id !== 'cleanup');
  if (mode === 'task' || mode === 'completion' || mode === 'notice') source = [...source,
    {id:'done', role:'tool', tool:'task_complete', content:'清理完成', ts:8000,
      metadata:{tool_summary:{verb:'complete', object:'task', metrics:[{label:'summary',value:'清理完成'}]}}},
  ];
  if (mode === 'notice') source = [...source, {id:'notice', role:'system', content:'后续系统通知',ts:9000}];
  const state = loadTranscriptHistory(createTranscriptState(), {messages:source,events:[]}).state;
  const items = projectCollapsedTranscriptItems(state.items);
  return <main data-fixture-mode={mode} className="bg-bg text-fg" style={{padding:24,minHeight:'100vh'}}>
    <div className="flex flex-col gap-3">
      <TranscriptItems items={items} expandedActivityKeys={expanded}
        capabilities={{showMessageFooters:false}}
        onToggleActivity={id => setExpanded(prev => {
          const next = new Set(prev); next.has(id) ? next.delete(id) : next.add(id); return next;
        })}/>
    </div>
  </main>;
}
createRoot(document.getElementById('root')).render(<App/>);
`;
const html = '<!doctype html><html><head><meta charset="UTF-8"/></head><body><div id="root"></div><script type="module" src="/@id/virtual:question-summary"></script></body></html>';
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  server: { host: '127.0.0.1', port: 0 },
  plugins: [{
    name: 'question-summary-fixture',
    resolveId(id) { if (id === 'virtual:question-summary') return '\0virtual:question-summary'; },
    async load(id) {
      if (id === '\0virtual:question-summary') {
        return (await transformWithEsbuild(entry, 'fixture.jsx', { loader: 'jsx' })).code;
      }
    },
    configureServer(vite) {
      vite.middlewares.use('/question-summary', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/question-summary', html));
      });
    },
  }],
});
const errors = [];
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  for (const [theme, width] of [['light', 1280], ['dark', 720]]) {
    const page = await browser.newPage({ viewport: { width, height: 1000 } });
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/question-summary`);
    await page.waitForFunction(() => !!window.fixture);
    await page.evaluate(theme => document.documentElement.dataset.theme = theme, theme);
    for (const mode of ['final', 'task', 'completion', 'notice']) {
      await page.evaluate(mode => window.fixture.mode(mode), mode);
      await page.waitForFunction(mode => document.querySelector('main')?.dataset.fixtureMode === mode, mode);
      const summaries = page.locator('.ace-activity-line-processed');
      const questions = page.locator('[data-ask-user-question-result="true"]');
      assert.equal(await summaries.count(), 1, mode);
      assert.match(await summaries.innerText(), /已处理 900m 27s/);
      assert.equal(await questions.count(), 2, mode);
      assert.match(await questions.nth(0).innerText(), /只清除 SDK 数据/);
      assert.match(await questions.nth(1).innerText(), /关了/);
      for (const index of [0, 1]) {
        const question = questions.nth(index);
        await question.getByRole('button').click();
        await question.locator('.ace-qa-answer').waitFor({ state: 'detached' });
        await question.getByRole('button').click();
        await question.locator('.ace-qa-answer').waitFor({ state: 'visible' });
      }
      if (mode === 'final') await page.screenshot({ path: path.join(output, theme + '-collapsed.png'), fullPage: true });
      const summaryControl = summaries.getByRole('button');
      await summaryControl.focus();
      await page.keyboard.press('Enter');
      const details = page.locator('[data-activity-details-reveal="true"]');
      await details.waitFor({ state: 'visible' });
      await page.locator('[data-activity-details-reveal="true"].is-settled').waitFor({ state: 'visible' });
      const expectedDetails = ['先确认清理范围', '检查应用状态',
        ...(mode === 'completion' ? [] : ['执行清理并检查结果'])];
      for (const expected of expectedDetails) {
        await details.getByText(expected, { exact: true }).waitFor({ state: 'visible' });
      }
      assert.equal(await details.locator('[data-ask-user-question-result="true"]').count(), 0);
      assert.equal(await questions.count(), 2);
      assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), 'horizontal overflow');
      if (mode === 'final') await page.screenshot({ path: path.join(output, theme + '-expanded.png'), fullPage: true });
      await summaryControl.click();
      await details.waitFor({ state: 'detached' });
    }
    await page.close();
  }
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ passed: 8, output, checks: ['single-duration', 'standalone-answers', 'keyboard-expansion', 'activity-details', 'no-overflow'] }));
} finally {
  await browser?.close();
  await server.close();
}
