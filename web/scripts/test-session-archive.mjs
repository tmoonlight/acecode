import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = process.env.ACE_ARCHIVE_ARTIFACTS || await fs.mkdtemp(path.join(os.tmpdir(), 'ace-archive-'));
await fs.mkdir(output, { recursive: true });
const fixture = String.raw`
import React,{useState} from 'react';
import {createRoot} from 'react-dom/client';
import {Sidebar} from '/src/components/Sidebar.jsx';
import {Toaster,toast} from '/src/components/Toast.jsx';
import {SettingsPage} from '/src/components/SettingsPage.jsx';
import {api} from '/src/lib/api.js';
import {connection} from '/src/lib/connection.js';
import '/src/i18n/index.js';
import '/src/styles/globals.css';
const workspaces=Array.from({length:9},(_,i)=>({hash:'w'+i,cwd:'/fixture/'+i,name:'项目 '+i,active:i===0}));
const all=workspaces.flatMap(w=>Array.from({length:15},(_,i)=>({id:w.hash+'-'+i,title:'会话 '+w.hash+' '+i,workspace_hash:w.hash,cwd:w.cwd,
 created_at:new Date(Date.UTC(2026,9,18,0,0,15-i)).toISOString(),active:false,attention_state:'read',sidebar_key:'session:'+w.hash+'-'+i})))
 .concat(Array.from({length:8},(_,i)=>({id:'task-'+i,title:'任务 '+i,no_workspace:true,workspace_hash:'',active:false,sidebar_key:'session:task-'+i})));
const archived=new Set();
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
window.fixture={archived,archiveCalls:[],undoCalls:[],pageCalls:[],pins:{},delay:180,failArchive:false,failUndo:false,failPage:false,holdNext:false,held:null,sections:[],toast,
 release(){this.held?.();this.held=null;},refresh(){window.dispatchEvent(new Event('ace-session-archive-changed'));}};
Object.assign(api,{
 listWorkspaces:async()=>workspaces,
 listSidebarSessions:async(hash,opts={})=>{
  window.fixture.pageCalls.push(hash);
  const rows=all.filter(s=>s.workspace_hash===hash&&!archived.has(s.id));
  const limit=opts.limit||rows.length;
  const result={sidebar_tree:true,sessions:rows.slice(0,limit).map((s,i)=>({...s,sidebar_rank:i})),total:rows.length,has_more:rows.length>limit,visible_roots:Math.min(limit,rows.length)};
  if(window.fixture.holdNext&&hash==='w0'){window.fixture.holdNext=false;await new Promise(resolve=>window.fixture.held=resolve);}
  else await wait(window.fixture.delay);
  if(window.fixture.failPage)throw Error('fixture list error');
  return result;
 },
 getPinnedSessions:async(hash)=>({session_ids:window.fixture.pins[hash]||[]}),getNoWorkspacePinnedSessions:async()=>({session_ids:[]}),getPinnedSessionOrder:async()=>({items:[]}),
 setPinnedSessions:async(hash,ids)=>{await wait(80);window.fixture.pins[hash]=ids;return {session_ids:ids};},setPinnedSessionOrder:async(items)=>({items}),
 getOpencodeImportPreview:async()=>({available:false,count:0}),
 archiveWorkspaceSession:async(hash,id)=>{window.fixture.archiveCalls.push([hash,id]);await wait(60);if(window.fixture.failArchive)throw Error('fixture archive error');archived.add(id);},
 archiveSession:async(id)=>api.archiveWorkspaceSession('',id),
 unarchiveWorkspaceSession:async(hash,id)=>{window.fixture.undoCalls.push([hash,id]);await wait(100);if(window.fixture.failUndo)throw Error('fixture undo error');archived.delete(id);},
 unarchiveSession:async(id)=>api.unarchiveWorkspaceSession('',id),
 listAllArchivedSessions:async()=>({workspaces,sessions:all.filter(s=>archived.has(s.id))}),
 listSessions:async()=>[],getConfig:async()=>({}),listProviders:async()=>[],listModelProfiles:async()=>[],
});
for(const name of ['reconfigure','retainSession','releaseSession','subscribeWorkspaceStatus','unsubscribeWorkspaceStatus'])connection[name]=()=>{};
function App(){const[section,setSection]=useState('');const[active,setActive]=useState(null);
 window.fixture.select=id=>setActive({sessionId:id,workspaceHash:id.startsWith('task')?'':'w0',noWorkspace:id.startsWith('task')});
 return <main className="flex h-screen bg-bg text-fg"><Sidebar width={300} activeRef={active} activeId={active?.sessionId} onSelect={setActive} onOpenHome={()=>setActive(null)}
  onOpenSettingsSection={key=>{window.fixture.sections.push(key);setSection(key);}}/><section className="flex-1"/>
  {section&&<SettingsPage initialNavKey={section} onClose={()=>setSection('')} office={{}}/>}<Toaster/></main>;}
createRoot(document.getElementById('root')).render(<App/>);
`;
const virtual = path.join(web, '__archive-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web, server: { host: '127.0.0.1', port: 0, open: false, hmr: false }, plugins: [{
  name: 'archive-fixture', enforce: 'pre',
  resolveId(id) { return id === '/__archive-fixture.jsx' ? virtual : undefined; },
  load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
  configureServer(vite) { vite.middlewares.use(async (req, res, next) => {
    if (req.url?.split('?')[0] !== '/archive-fixture') return next();
    res.setHeader('Content-Type', 'text/html');
    res.end(await vite.transformIndexHtml('/archive-fixture', '<!doctype html><html lang="zh-CN" data-theme="light"><meta name="viewport" content="width=device-width,initial-scale=1"><body><div id="root"></div><script type="module" src="/__archive-fixture.jsx"></script></body></html>'));
  }); },
}] });
let browser;
let page;
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, ...(process.env.ACE_CHROMIUM_EXECUTABLE ? { executablePath: process.env.ACE_CHROMIUM_EXECUTABLE } : {}) });
  page = await browser.newPage({ viewport: { width: 1100, height: 700 }, reducedMotion: 'reduce' });
  // Ancillary settings/navigation fetches are outside this archive fixture.
  await page.route('**/api/**', route => route.fulfill({ json: {} }));
  page.on('pageerror', error => errors.push(error.message));
  const row = id => page.locator('.ace-sidebar-session-row[data-desktop-session-id="' + id + '"]');
  const rows = () => page.locator('.ace-sidebar-session-row[data-desktop-session-workspace="w0"]');
  const archive = async id => { await row(id).hover(); await row(id).getByRole('button', { name: '归档', exact: true }).click(); };
  await page.goto(server.resolvedUrls.local[0] + 'archive-fixture');
  await row('w0-0').waitFor();
  await page.evaluate(() => {
    window.frames = [];
    window.recording = true;
    const record = () => {
      window.frames.push({ count: document.querySelectorAll('.ace-sidebar-session-row[data-desktop-session-workspace="w0"]').length,
        scroll: document.querySelector('.ace-sidebar-scroll').scrollTop });
      if (window.recording) requestAnimationFrame(record);
    };
    record();
  });
  await archive('w0-0');
  await row('w0-5').waitFor();
  const frames = await page.evaluate(() => { window.recording = false; return window.frames; });
  const counts = [...new Set(frames.map(frame => frame.count))];
  if (process.env.ACE_ARCHIVE_BASELINE === '1') {
    console.log(JSON.stringify({ baselineRowCounts: counts, frames: frames.length, errors }));
  } else {
    assert.deepEqual(counts, [5], 'archive and backfill must commit in one render');
    assert.equal(await rows().count(), 5);
    const notice = page.getByRole('status').filter({ has: page.getByRole('button', { name: '撤销', exact: true }) });
    await notice.first().waitFor();
    const color = await notice.first().getByRole('button', { name: '撤销', exact: true }).evaluate(el => {
      const probe = document.createElement('span'); probe.style.color = 'var(--ace-accent)'; el.append(probe);
      const result = { color: getComputedStyle(el).color, background: getComputedStyle(el).backgroundColor, accent: getComputedStyle(probe).color }; probe.remove(); return result;
    });
    assert.equal(color.background, color.accent, 'undo background uses the active theme accent');
    assert.equal(color.color, 'rgb(255, 255, 255)', 'undo has white text');
    assert.deepEqual(await notice.first().getByRole('button').evaluateAll(buttons => buttons.map(button => button.getAttribute('aria-label') || button.textContent)), ['查看', '撤销', '关闭提示']);
    assert.equal(await notice.first().locator('[data-icon-name="Archive"]').count(), 1);
    assert.match(await notice.first().textContent(), /已归档的聊天/);
    await page.evaluate(() => fixture.toast({ text: '样式对照', duration: 0, dismissible: true }));
    const plainNotice = page.getByRole('status').filter({ hasText: '样式对照' });
    const noticeStyle = el => {
      const style = getComputedStyle(el);
      const message = getComputedStyle(el.children[1]);
      return { font: message.fontFamily, size: message.fontSize, weight: message.fontWeight, lineHeight: message.lineHeight,
        radius: style.borderRadius, padding: style.padding, height: el.getBoundingClientRect().height };
    };
    assert.deepEqual(await notice.first().evaluate(noticeStyle), await plainNotice.evaluate(noticeStyle), 'archive keeps the ordinary toast typography, shape and height');
    assert.equal(await notice.first().getByRole('button', { name: '撤销', exact: true }).evaluate(el => getComputedStyle(el).fontSize), await plainNotice.evaluate(el => getComputedStyle(el).fontSize), 'buttons follow the existing toast font-size preference');
    await plainNotice.getByRole('button', { name: '关闭提示', exact: true }).click();
    await page.screenshot({ path: path.join(output, 'archive-toast-light.png') });
    await notice.first().screenshot({ path: path.join(output, 'archive-toast-blue.png') });
    await page.evaluate(() => document.documentElement.style.setProperty('--ace-accent', '#0d0d0d'));
    await page.waitForFunction(() => getComputedStyle(document.querySelector('[role="status"] .bg-accent')).backgroundColor === 'rgb(13, 13, 13)');
    assert.equal(await notice.first().getByRole('button', { name: '撤销', exact: true }).evaluate(el => getComputedStyle(el).backgroundColor), 'rgb(13, 13, 13)');
    await notice.first().screenshot({ path: path.join(output, 'archive-toast-black.png') });
    await page.evaluate(() => document.documentElement.style.removeProperty('--ace-accent'));
    await archive('w0-1');
    await row('w0-6').waitFor();
    await notice.first().getByRole('button', { name: '撤销', exact: true }).click();
    await row('w0-1').waitFor();
    assert.deepEqual(await page.evaluate(() => fixture.undoCalls), [['w0', 'w0-1']], 'undo binds to its own notice');
    assert.equal(await row('w0-0').count(), 0);
    await notice.first().getByRole('button', { name: '查看', exact: true }).click();
    await page.locator('[data-archived-filters]').waitFor();
    assert.deepEqual(await page.evaluate(() => fixture.sections), ['archived']);
    await page.screenshot({ path: path.join(output, 'archive-view.png') });

    const reset = async () => {
      await page.reload(); await row('w0-0').waitFor();
      await page.waitForTimeout(250);
    };
    await reset();
    await archive('w0-0');
    await row('w0-5').waitFor();
    await notice.first().getByRole('button', { name: '关闭提示', exact: true }).click();
    assert.equal(await notice.count(), 0);
    assert.deepEqual(await page.evaluate(() => ({ archived: [...fixture.archived], undoCalls: fixture.undoCalls, sections: fixture.sections })), { archived: ['w0-0'], undoCalls: [], sections: [] }, 'dismiss only hides the notification');
    await reset();
    await archive('w0-0');
    await row('w0-5').waitFor();
    await archive('w0-1');
    await row('w0-6').waitFor();
    await notice.first().getByRole('button', { name: '查看', exact: true }).click();
    await page.locator('[data-archived-filters]').waitFor();
    await page.getByRole('checkbox', { name: '选择会话 会话 w0 0', exact: true }).waitFor();
    await notice.first().getByRole('button', { name: '撤销', exact: true }).click();
    await page.getByRole('checkbox', { name: '选择会话 会话 w0 0', exact: true }).waitFor({ state: 'detached' });
    assert.equal(await page.getByRole('checkbox', { name: '选择会话 会话 w0 1', exact: true }).count(), 1, 'open archive page follows toast undo');

    await reset();
    await page.evaluate(() => { fixture.failArchive = true; });
    await archive('w0-0');
    await page.getByRole('status').filter({ hasText: '归档失败:' }).waitFor();
    assert.equal(await row('w0-0').count(), 1, 'failed archive retains its row');
    assert.equal(await notice.count(), 0);
    await page.evaluate(() => { fixture.failArchive = false; fixture.archiveCalls = []; });
    await row('w0-0').getByRole('button', { name: '归档', exact: true }).evaluate(el => { el.click(); el.click(); });
    await row('w0-5').waitFor();
    assert.deepEqual(await page.evaluate(() => fixture.archiveCalls), [['w0', 'w0-0']], 'duplicate click sends one archive');
    await page.evaluate(() => { fixture.failUndo = true; });
    await notice.first().getByRole('button', { name: '撤销', exact: true }).click();
    await page.getByRole('status').filter({ hasText: '撤销归档失败' }).waitFor();
    assert.equal(await row('w0-0').count(), 0);
    await page.evaluate(() => { fixture.failUndo = false; fixture.undoCalls = []; });
    await notice.first().getByRole('button', { name: '撤销', exact: true }).evaluate(el => { el.click(); el.click(); });
    await row('w0-0').waitFor();
    assert.deepEqual(await page.evaluate(() => fixture.undoCalls), [['w0', 'w0-0']], 'failed undo can retry without duplicate submission');

    await reset();
    await page.evaluate(() => { fixture.holdNext = true; fixture.refresh(); });
    await page.waitForFunction(() => Boolean(fixture.held));
    await archive('w0-0');
    await row('w0-5').waitFor();
    await page.evaluate(() => {
      window.resurrected = false;
      new MutationObserver(() => {
        if (document.querySelector('[data-desktop-session-id="w0-0"]')) window.resurrected = true;
      }).observe(document.querySelector('.ace-sidebar-scroll'), { childList: true, subtree: true });
      fixture.release();
    });
    await page.waitForTimeout(650);
    assert.equal(await page.evaluate(() => window.resurrected), false, 'old polling response must never resurrect archived rows');

    await reset();
    await page.evaluate(() => {
      for (const id of ['w0-0', 'w0-1']) document.querySelector('[data-desktop-session-id="'+id+'"] button[title="归档"]').click();
    });
    await row('w0-6').waitFor();
    assert.equal(await row('w0-0').count(), 0);
    assert.equal(await row('w0-1').count(), 0);
    assert.equal(await rows().count(), 5, 'overlapping archive requests keep a full page');
    await notice.last().getByRole('button', { name: '撤销', exact: true }).click();
    await row('w0-0').waitFor();
    assert.equal(await row('w0-1').count(), 0, 'an older toast still restores its original target');

    await reset();
    await archive('task-0');
    await page.waitForFunction(() => !document.querySelector('[data-desktop-session-id="task-0"]'));
    await notice.first().getByRole('button', { name: '撤销', exact: true }).click();
    await row('task-0').waitFor();
    assert.deepEqual(await page.evaluate(() => fixture.undoCalls), [['', 'task-0']], 'no-workspace undo uses the compatibility API');

    await reset();
    await page.evaluate(() => { fixture.pins.w0 = ['w0-0']; fixture.refresh(); });
    await page.locator('[data-desktop-session-id="w0-0"][data-desktop-session-pinned="true"]').waitFor();
    await page.evaluate(() => {
      window.unpinnedFlash = false;
      new MutationObserver(() => {
        const archivedRow = document.querySelector('[data-desktop-session-id="w0-0"]');
        if (archivedRow && archivedRow.getAttribute('data-desktop-session-pinned') !== 'true') window.unpinnedFlash = true;
      }).observe(document.querySelector('.ace-sidebar-scroll'), { childList: true, subtree: true, attributes: true });
    });
    await archive('w0-0');
    await notice.first().waitFor();
    assert.equal(await page.evaluate(() => window.unpinnedFlash), false, 'archived pinned row never flashes in ordinary sessions');
    assert.equal(await rows().count(), 5);
    await notice.first().getByRole('button', { name: '撤销', exact: true }).click();
    await row('w0-0').waitFor();
    assert.notEqual(await row('w0-0').getAttribute('data-desktop-session-pinned'), 'true');

    await reset();
    await page.evaluate(() => { fixture.select('w0-0'); });
    await archive('w0-0');
    await row('w0-5').waitFor();
    assert.equal(await page.locator('[data-desktop-session-id="w0-0"]').count(), 0);

    await reset();
    // Expand all workspaces so the same mutation can be checked while scrolled.
    for (let index = 1; index < 9; index += 1) {
      await page.locator('[data-desktop-workspace-id="w'+index+'"]').evaluate(el => el.click());
    }
    await row('w8-0').waitFor();
    await row('w4-0').hover();
    const scroll = await page.locator('.ace-sidebar-scroll').evaluate(el => el.scrollTop);
    await archive('w4-0');
    await row('w4-5').waitFor();
    assert.equal(await page.locator('.ace-sidebar-scroll').evaluate(el => el.scrollTop), scroll, 'archive backfill keeps the scrolled viewport stable');

    await reset();
    for (const interaction of ['hover', 'focus']) {
      await page.evaluate(() => fixture.toast({ text: '计时验证', duration: 150, actions: [{ label: '测试操作', onClick: () => {} }] }));
      const timerToast = page.getByRole('status').filter({ hasText: '计时验证' });
      if (interaction === 'hover') await timerToast.hover();
      else await timerToast.getByRole('button').focus();
      await page.waitForTimeout(350);
      assert.equal(await timerToast.count(), 1, interaction + ' pauses expiration');
      if (interaction === 'hover') await page.mouse.move(0, 699);
      else await timerToast.getByRole('button').evaluate(el => el.blur());
      await timerToast.waitFor({ state: 'detached' });
    }
    await page.evaluate(() => fixture.toast({ text: '异步计时验证', duration: 150, actions: [{ label: '执行操作', onClick: () => new Promise(resolve => setTimeout(resolve, 500)) }] }));
    const busyToast = page.getByRole('status').filter({ hasText: '异步计时验证' });
    await busyToast.getByRole('button').click();
    await page.mouse.move(0, 699);
    await page.waitForTimeout(250);
    assert.equal(await busyToast.count(), 1, 'pending action pauses expiration');
    assert.equal(await busyToast.getByRole('button').isDisabled(), true);
    await busyToast.waitFor({ state: 'detached' });

    await archive('w0-0');
    await row('w0-5').waitFor();
    await page.setViewportSize({ width: 360, height: 740 });
    await page.evaluate(() => document.documentElement.setAttribute('data-theme', 'dark'));
    const bounds = await notice.first().boundingBox();
    assert.ok(bounds.x >= 0 && bounds.x + bounds.width <= 360, 'action toast fits narrow screens');
    assert.equal(await notice.first().getByRole('button', { name: '撤销', exact: true }).isVisible(), true);
    assert.equal(await notice.first().getByRole('button', { name: '查看', exact: true }).isVisible(), true);
    await page.screenshot({ path: path.join(output, 'archive-toast-dark-narrow.png') });
    assert.deepEqual(errors, []);
    console.log(JSON.stringify({ passed: true, counts, color, checks: ['atomic backfill', 'undo target', 'view route', 'duplicate archive', 'failure/retry', 'stale response', 'concurrent archive', 'no-workspace undo', 'active session', 'scroll stability', 'hover/focus/pending timer', 'narrow dark'], output }));
  }
} catch (error) {
  console.error(JSON.stringify({ errors, notices: await page?.getByRole('status').allTextContents(), state: await page?.evaluate(() => ({ archiveCalls: fixture.archiveCalls, undoCalls: fixture.undoCalls, archived: [...fixture.archived] })) }));
  await page?.screenshot({ path: path.join(output, 'failure.png') });
  throw error;
} finally {
  await browser?.close();
  await server.close();
}
