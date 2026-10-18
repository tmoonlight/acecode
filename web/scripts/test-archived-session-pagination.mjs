import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = process.env.ACE_ARCHIVE_ARTIFACTS || await fs.mkdtemp(path.join(os.tmpdir(), 'ace-archive-pagination-'));
await fs.mkdir(output, { recursive: true });
const fixture = `
import React,{useState} from 'react';
import {createRoot} from 'react-dom/client';
import {SettingsPage} from '/src/components/SettingsPage.jsx';
import {Toaster} from '/src/components/Toast.jsx';
import {api} from '/src/lib/api.js';
import '/src/i18n/index.js';
import '/src/styles/globals.css';
const workspaces=[{hash:'a',name:'工作区 A',cwd:'/fixture/a'},{hash:'b',name:'工作区 B',cwd:'/fixture/b'}];
const makeRows=(count,hash)=>Array.from({length:count},(_,i)=>({id:hash+'-'+i,title:'归档 '+hash+' '+i,workspace_hash:hash,
 workspaceName:hash==='a'?'工作区 A':'工作区 B',cwd:'/fixture/'+hash,
 updated_at:new Date(Date.UTC(2026,9,18,0,0,-i)).toISOString()}));
window.fixture={rows:[...Array.from({length:5},(_,i)=>({id:'task-'+i,title:'任务 '+i,no_workspace:true})),...makeRows(2200,'a'),...makeRows(100,'b')],calls:[],failId:'',loads:0,
 refresh(){window.dispatchEvent(new CustomEvent('ace-session-archive-changed',{detail:{source:'fixture'}}));}};
const remove=async(hash,id)=>{window.fixture.calls.push([hash,id]);if(id===window.fixture.failId)throw Error('fixture failure');
 window.fixture.rows=window.fixture.rows.filter(s=>!(s.id===id&&(!hash||s.workspace_hash===hash)));};
Object.assign(api,{
 listAllArchivedSessions:async()=>{window.fixture.loads++;return {workspaces,sessions:[...window.fixture.rows]};},
 listWorkspaces:async()=>workspaces,listSessions:async()=>[],getConfig:async()=>({}),listProviders:async()=>[],listModelProfiles:async()=>[],
 unarchiveWorkspaceSession:remove,unarchiveSession:id=>remove('',id),purgeArchivedWorkspaceSession:remove,purgeArchivedSession:id=>remove('',id),
});
function App(){const[open,setOpen]=useState(true);window.fixture.open=setOpen;return <main className="h-screen bg-bg text-fg">
 {open&&<SettingsPage initialNavKey="archived" onClose={()=>setOpen(false)} office={{}}/>}<Toaster/></main>;}
createRoot(document.getElementById('root')).render(<App/>);
`;
const virtual = path.join(web, '__archive-pagination-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web, server: { host: '127.0.0.1', port: 0, open: false, hmr: false }, plugins: [{
  name: 'archive-pagination-fixture', enforce: 'pre',
  resolveId(id) { return id === '/__archive-pagination-fixture.jsx' ? virtual : undefined; },
  load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
  configureServer(vite) { vite.middlewares.use(async (req, res, next) => {
    if (req.url?.split('?')[0] !== '/archive-pagination-fixture') return next();
    res.setHeader('Content-Type', 'text/html');
    res.end(await vite.transformIndexHtml('/archive-pagination-fixture', '<!doctype html><html lang="zh-CN" data-theme="light"><meta name="viewport" content="width=device-width,initial-scale=1"><body><div id="root"></div><script type="module" src="/__archive-pagination-fixture.jsx"></script></body></html>'));
  }); },
}] });
let browser;
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, ...(process.env.ACE_CHROMIUM_EXECUTABLE ? { executablePath: process.env.ACE_CHROMIUM_EXECUTABLE } : {}) });
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 }, reducedMotion: 'reduce' });
  await page.route('**/api/**', route => route.fulfill({ json: {} }));
  page.on('pageerror', error => errors.push(error.message));
  const rows = page.locator('.ace-settings-content ul > li');
  const count = async expected => page.waitForFunction(n => document.querySelectorAll('.ace-settings-content ul > li').length === n, expected);
  const bottom = async () => page.locator('.ace-settings-content').evaluate(el => { el.scrollTop = el.scrollHeight; });
  const top = async () => page.locator('.ace-settings-content').evaluate(el => { el.scrollTop = 0; });
  const search = page.getByRole('searchbox', { name: '搜索已归档会话' });
  const workspace = page.getByRole('combobox', { name: '选择工作区' });
  const sort = page.getByRole('combobox', { name: '排序方式' });
  const start = Date.now();
  await page.goto(server.resolvedUrls.local[0] + 'archive-pagination-fixture');
  await rows.first().waitFor();
  const metrics = { rows: await rows.count(), elements: await page.locator('.ace-settings-content *').count(), fixtureReadyMs: Date.now() - start };
  if (process.env.ACE_ARCHIVE_BASELINE === '1') {
    assert.equal(metrics.rows, 2305);
    console.log(JSON.stringify({ baseline: metrics, errors }));
  } else {
    await count(100);
    await page.getByRole('button', { name: '全选', exact: true }).click();
    assert.equal(await page.locator('.ace-settings-content input:checked').count(), 100);
    await bottom(); await count(200);
    assert.equal(await page.locator('.ace-settings-content input:checked').count(), 100, 'new batch is not auto selected');
    await top(); await search.fill('归档 a 2199'); await count(1);
    assert.equal(await page.locator('.ace-settings-content input:checked').count(), 0);
    assert.match(await rows.first().innerText(), /a 2199/);
    await search.fill(''); await count(100);
    await sort.selectOption('oldest'); await count(100);
    assert.match(await rows.nth(5).innerText(), /a 2199/);
    await workspace.selectOption('b'); await count(100);
    assert.equal(await page.locator('[data-archived-load-more]').count(), 0, 'exactly 100 needs no sentinel');
    await workspace.selectOption('a'); await count(100);
    await bottom(); await count(200);
    await top(); await sort.selectOption('newest'); await count(100);
    await page.getByRole('button', { name: '全选', exact: true }).click();
    await page.evaluate(() => { window.fixture.failId = 'a-0'; });
    await page.getByRole('button', { name: '取消选中会话的归档', exact: true }).click();
    await page.waitForFunction(() => window.fixture.calls.length === 100);
    await page.waitForFunction(() => document.querySelectorAll('.ace-settings-content input:checked').length === 1);
    await count(100);
    assert.equal(await page.locator('.ace-settings-content input:checked').count(), 1, 'failed row stays selected');
    assert.equal(await page.evaluate(() => window.fixture.rows.filter(s => s.workspace_hash === 'a').length), 2101);
    await bottom(); await count(200);
    await page.evaluate(() => { window.fixture.rows=window.fixture.rows.filter(s=>s.id!=='a-100');window.fixture.refresh(); });
    await page.waitForFunction(() => window.fixture.loads === 2);
    await count(200);
    await top(); await workspace.selectOption('b'); await count(100);
    await page.evaluate(() => { window.fixture.rows.push({id:'b-last',title:'尾批唯一会话',workspace_hash:'b'});window.fixture.refresh(); });
    await page.waitForFunction(() => window.fixture.loads === 3);
    await bottom(); await count(101);
    assert.equal(await page.locator('[data-archived-load-more]').count(), 0);
    assert.match(await rows.last().innerText(), /尾批唯一会话/);
    // Reset to the first 100 of 101, then verify destructive actions never reach
    // the unmounted tail and cancelling the confirmation does not send requests.
    await top(); await sort.selectOption('oldest'); await count(100);
    const beforeDelete = await page.evaluate(() => window.fixture.calls.length);
    await page.getByRole('button', { name: '全选', exact: true }).click();
    await page.getByRole('button', { name: '删除选中会话', exact: true }).click();
    await page.getByRole('button', { name: '取消', exact: true }).click();
    assert.equal(await page.evaluate(() => window.fixture.calls.length), beforeDelete);
    await page.getByRole('button', { name: '删除选中会话', exact: true }).click();
    await page.getByRole('button', { name: '彻底删除', exact: true }).click();
    await count(1);
    assert.equal(await page.evaluate(() => window.fixture.calls.length), beforeDelete + 100);
    assert.match(await rows.first().innerText(), /尾批唯一会话/);
    await top(); await workspace.selectOption(''); await count(100);
    await page.screenshot({ path: path.join(output, 'archive-pagination-desktop.png') });
    await page.setViewportSize({ width: 430, height: 800 });
    await page.evaluate(() => document.documentElement.setAttribute('data-theme','dark'));
    assert.equal(await page.locator('.ace-settings-content').evaluate(el => el.scrollWidth > el.clientWidth), false);
    await page.screenshot({ path: path.join(output, 'archive-pagination-narrow.png') });
    // Reopen without IntersectionObserver to exercise the older-WebView fallback.
    await page.evaluate(() => { window.fixture.open(false); });
    await page.locator('.ace-settings-content').waitFor({ state: 'detached' });
    await page.evaluate(() => { window.IntersectionObserver=undefined;window.fixture.open(true); });
    await count(100); await bottom(); await count(200);
    await top(); await search.fill('no such archive'); await count(0);
    await page.getByRole('status').filter({ hasText: '没有匹配的已归档会话' }).waitFor();
    assert.deepEqual(errors, []);
    console.log(JSON.stringify({ after: metrics, checked: ['2305-row initial cap','scroll batches','full search','sort reset','100/101 boundary','selection','partial restore failure','backfill','external refresh','delete confirmation and visible-only targets','reopen','observer fallback','narrow dark layout'], output, errors }));
  }
} finally {
  await browser?.close();
  await server.close();
}
