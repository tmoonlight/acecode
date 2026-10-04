import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

// Real page, manual form, icons and rich composer. No live scheduler is touched.
const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const output = process.env.ACE_SCHEDULE_ARTIFACTS || await fs.mkdtemp(path.join(os.tmpdir(), 'ace-scheduled-ui-'));
await fs.mkdir(output, { recursive: true });
const fixture = String.raw`
import React,{useState} from 'react';
import {createRoot} from 'react-dom/client';
import {LoopPage} from '/src/components/LoopPage.jsx';
import {RichComposer} from '/src/components/RichComposer.jsx';
import {scheduledTaskCreationRef} from '/src/lib/scheduledTaskCreation.js';
import {commandsWithFallback} from '/src/lib/slashCommands.js';
import {api} from '/src/lib/api.js';
import '/src/styles/globals.css';
const tasks=[];
window.mutations=[];window.submissions=0;
Object.assign(api,{
 listLoops:async()=>({loops:[...tasks]}),listWorkspaces:async()=>[],
 listModels:async()=>({models:[{name:'test-model'}]}),getDefaultModel:async()=>({name:'test-model'}),
 createLoop:async(body)=>{window.mutations.push(body);const task={...body,id:'created',next_run_at_ms:Date.now()+3600000};tasks.push(task);return task;},
});
const commands=commandsWithFallback({skills:[{name:'scheduled-task',description:'创建定时任务'}]});
function Fixture(){
 const [route,setRoute]=useState(null),[text,setText]=useState('');
 const smart=()=>{const next=scheduledTaskCreationRef({loop:true},{});window.next=next;setText(next.initialDraftText);setRoute(next);};
 window.fixture={reset:()=>setRoute(null),get text(){return text;}};
 return <main className="h-screen flex flex-col text-fg bg-bg">
  {route?<section className="max-w-[760px] w-full mx-auto p-6">
   <h1 className="text-[20px] font-semibold mb-5">新建任务</h1>
   <div className="rounded-lg border border-border bg-surface p-3">
    <RichComposer value={text} onChange={setText} commands={commands} aria-label="任务内容" className="min-h-[96px] text-[14px]" onSubmit={()=>window.submissions++}/>
   </div>
  </section>:<LoopPage onSmartAdd={smart} onOpenSession={()=>{}}/>}
 </main>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const virtual = path.join(web, '__scheduled-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({
  root: web, server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{
    name: 'scheduled-task-fixture',
    enforce: 'pre',
    resolveId(id) { return id === '/__scheduled-fixture.jsx' ? virtual : undefined; },
    load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
    configureServer(vite) {
      vite.middlewares.use(async (request, response, next) => {
        if (request.url?.split('?')[0] !== '/scheduled-fixture') return next();
        response.setHeader('Content-Type', 'text/html');
        response.end(await vite.transformIndexHtml('/scheduled-fixture', '<!doctype html><html lang="zh-CN" data-theme="light"><meta name="viewport" content="width=device-width, initial-scale=1"><body><div id="root"></div><script type="module" src="/__scheduled-fixture.jsx"></script></body></html>'));
      });
    },
  }],
});
let browser;
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, ...(process.env.ACE_CHROMIUM_EXECUTABLE ? { executablePath: process.env.ACE_CHROMIUM_EXECUTABLE } : {}) });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/scheduled-fixture`);
  const smart = page.getByRole('button', { name: '智能添加定时任务', exact: true });
  const manual = page.getByRole('button', { name: '手动添加', exact: true });
  await smart.waitFor();
  assert.equal(await smart.locator('svg').count(), 1);
  assert.ok((await smart.boundingBox()).x < (await manual.boundingBox()).x);
  for (const theme of ['light', 'dark']) {
    for (const width of [1280, 390]) {
      await page.setViewportSize({ width, height: 800 });
      await page.evaluate(mode => { document.documentElement.dataset.theme = mode; }, theme);
      assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth));
      assert.ok(await smart.isVisible());
      assert.ok(await manual.isVisible());
      await page.screenshot({ path: path.join(output, `${theme}-${width}.png`), animations: 'disabled' });
    }
  }
  await page.setViewportSize({ width: 1280, height: 800 });
  await manual.click();
  const dialog = page.getByRole('dialog');
  await dialog.getByRole('heading', { name: '添加定时任务' }).waitFor();
  await dialog.locator('input').first().fill('Browser reminder');
  await dialog.locator('textarea').fill('Remind me to attend the meeting.');
  assert.equal(await dialog.getByRole('combobox', { name: '权限模式' }).inputValue(), 'yolo');
  await dialog.getByRole('button', { name: '添加定时任务', exact: true }).click();
  await page.getByRole('heading', { name: 'Browser reminder' }).waitFor();
  assert.equal(await page.evaluate(() => window.mutations.length), 1);
  assert.doesNotMatch(await page.locator('body').innerText(), /循环|LOOP/);
  await smart.focus();
  await page.keyboard.press('Enter');
  const composer = page.getByRole('textbox', { name: '任务内容' });
  await composer.waitFor();
  await page.locator('[data-slash-chip-kind="skill"]').waitFor();
  assert.equal(await page.evaluate(() => window.fixture.text), '/scheduled-task 我希望在明天X点提醒我参加会议，重复X天');
  assert.equal(await page.evaluate(() => window.submissions), 0);
  assert.equal(await page.evaluate(() => window.mutations.length), 1);
  await composer.click();
  await page.keyboard.press('End');
  await page.keyboard.type(' edited');
  assert.ok((await page.evaluate(() => window.fixture.text)).endsWith(' edited'));
  await page.screenshot({ path: path.join(output, 'smart-draft.png'), animations: 'disabled' });
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ result: 'passed', output, checks: ['button order and SVG', 'light/dark at 1280/390', 'manual form and save', 'keyboard smart entry', 'skill chip', 'exact editable unsent draft'], errors }));
} finally {
  if (errors.length) console.error(JSON.stringify({ errors }));
  await browser?.close();
  await server.close();
}
