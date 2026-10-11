import assert from 'node:assert/strict';
import { mkdtemp } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer, transformWithEsbuild } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await mkdtemp(path.join(os.tmpdir(), 'ace-title-animation-'));
const entry = `
import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import {SessionTitleBar} from '/src/components/SessionTitleBar.jsx';
import {SessionRow} from '/src/components/Sidebar.jsx';
import {sessionDisplayTitle} from '/src/lib/sessionTitle.js';
import '/src/styles/globals.css';
function App() {
  const [state, setState] = useState({id:'one', title:'新会话1', ready:true, portal:false, renaming:false});
  const patch = values => flushSync(() => setState(s => ({...s, ...values})));
  window.fixture = {patch};
  const s = {id:state.id, workspace_hash:'workspace', title:state.title};
  return <main style={{padding:16}}>
    <div className="ace-topbar flex min-w-0 items-center justify-between" style={{height:36}}>
      <div id="title-target" className="min-w-0"/><div id="actions-target"/>
    </div>
    <SessionTitleBar title={sessionDisplayTitle(s)} sessionRef={{sessionId:s.id, workspaceHash:'workspace', port:28080}}
      titleReady={state.ready} workspaceLabel="ACECode" renaming={state.renaming}
      titleTarget={state.portal ? document.getElementById('title-target') : null}
      actionsTarget={state.portal ? document.getElementById('actions-target') : null}
      onRenameCancel={() => patch({renaming:false})}
      onRenameCommit={title => patch({title, renaming:false})}>
      <button aria-label="Action">...</button>
    </SessionTitleBar>
    <aside className="bg-surface border border-edge rounded-md" style={{width:'min(270px, 100%)',marginTop:24}}>
      <SessionRow key={s.id} s={s} active onSelect={() => {}} onTogglePin={() => {}}
        onArchive={() => {}} onRename={(_session,title) => patch({title})}/>
    </aside>
  </main>;
}
createRoot(document.getElementById('root')).render(<App/>);
if (window.aceFocusTest) {
  (async () => {
    const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
    try {
      while (!window.fixture) await wait(20);
      await aceFocusTest('show');
      await wait(100);
      const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;
      const read = () => [...document.querySelectorAll('.ace-session-title-transition')].map(el => ({
        typing:el.dataset.titleTyping,
        text:el.querySelector('.ace-session-title-typing')?.textContent,
        visible:el.querySelector('.ace-session-title-typing')
          ? getComputedStyle(el.querySelector('.ace-session-title-typing')).display !== 'none' : false,
      }));
      fixture.patch({title:'当前Windows设置下顶部和侧栏仍然同步逐字更新'});
      await wait(80);
      const partial = read();
      if (partial.length !== 2 || partial.some(p => p.typing !== 'true' || !p.visible || p.text.length <= 1)
        || partial[0].text !== partial[1].text) throw new Error(JSON.stringify(partial));
      await wait(700);
      if (document.querySelector('.ace-session-title-cursor')) throw new Error('Cursor remains after completion');
      await aceFocusTest('done', true, {reducedMotion, partial, completedWithoutCursor:true, userAgent:navigator.userAgent});
    } catch (error) { await aceFocusTest('done', false, {error:String(error)}); }
  })();
}
`;
const html = '<!doctype html><html><head><meta charset="UTF-8"/></head><body><div id="root"></div><script type="module" src="/@id/virtual:title-animation"></script></body></html>';
const syncEntry = `
import React, {useEffect, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {SessionTitleBar} from '/src/components/SessionTitleBar.jsx';
import {SessionRow} from '/src/components/Sidebar.jsx';
import {useSessionTranscript} from '/src/lib/sessionTranscript.js';
import {connection} from '/src/lib/connection.js';
import '/src/styles/globals.css';
const ref = {sessionId:'title-race', workspaceHash:'fixture', title:'原标题', active:true};
const pending = [];
const originalFetch = window.fetch;
window.fetch = (url, options) => String(url).includes('/messages?')
  ? new Promise(resolve => pending.push(data => resolve(new Response(JSON.stringify(data), {headers:{'Content-Type':'application/json'}}))))
  : originalFetch(url, options);
for (const key of ['reconfigure','retainSession','releaseSession']) connection[key] = () => {};
window.fixture = {
  resolve(index, data) { pending[index](data); },
  count() { return pending.length; },
  emit(type, seq, payload) { connection.dispatchEvent(new CustomEvent('message', {detail:{session_id:ref.sessionId,type,seq,payload}})); },
};
function App() {
  const transcript = useSessionTranscript(ref, {live:true});
  const [session, setSession] = useState({id:ref.sessionId, workspace_hash:'fixture', title:ref.title});
  useEffect(() => {
    const handler = e => { if (e.detail.type === 'session_updated') setSession(s => ({...s, ...e.detail.payload})); };
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, []);
  return <main style={{padding:24}}>
    <SessionTitleBar title={transcript.title} sessionRef={ref} titleReady={transcript.loadState === 'loaded'}/>
    <aside style={{width:280,marginTop:24}}><SessionRow s={session} active onSelect={()=>{}} onTogglePin={()=>{}} onArchive={()=>{}}/></aside>
  </main>;
}
createRoot(document.getElementById('root')).render(<App/>);
`;
const server = await createServer({
  root: web, logLevel: 'error',
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{
    name: 'title-animation-fixture', enforce: 'pre',
    resolveId(id) { if (id === 'virtual:title-animation' || id === 'virtual:title-sync') return '\0' + id; },
    async load(id) {
      if (id === '\0virtual:title-animation') return (await transformWithEsbuild(entry, 'fixture.jsx', {loader:'jsx'})).code;
      if (id === '\0virtual:title-sync') return (await transformWithEsbuild(syncEntry, 'sync-fixture.jsx', {loader:'jsx'})).code;
    },
    transform(code, id) {
      // Exercise the actual private sidebar row without changing its production exports.
      if (id.replaceAll('\\', '/').endsWith('/components/Sidebar.jsx')) return code + '\nexport { SessionRow };';
    },
    configureServer(vite) {
      vite.middlewares.use('/title-sync', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/title-sync', html.replace('virtual:title-animation', 'virtual:title-sync')));
      });
      vite.middlewares.use('/title-animation', async (_req, res) => {
        res.setHeader('Content-Type', 'text/html');
        res.end(await vite.transformIndexHtml('/title-animation', html));
      });
    },
  }],
});
let browser;
try {
  await server.listen();
  if (process.env.ACE_TITLE_NATIVE_HOST) {
    const profile = await mkdtemp(path.join(os.tmpdir(), 'ace-title-webview-'));
    await new Promise((resolve, reject) => {
      const child = spawn(process.env.ACE_TITLE_NATIVE_HOST,
        [`http://127.0.0.1:${server.httpServer.address().port}/title-animation`],
        {windowsHide:true, env:{...process.env, WEBVIEW2_USER_DATA_FOLDER:profile}, stdio:'inherit'});
      const timer = setTimeout(() => { child.kill(); reject(new Error('Native title test timed out')); }, 30000);
      child.on('error', error => {clearTimeout(timer); reject(error);});
      child.on('exit', code => {clearTimeout(timer); code === 0 ? resolve() : reject(new Error('Native title test exit '+code));});
    });
  } else {
    browser = await chromium.launch({headless:true, executablePath:process.env.ACE_CHROMIUM_EXECUTABLE || undefined});
    const page = await browser.newPage({viewport:{width:1200,height:500}});
    const errors = [];
    page.on('pageerror', error => errors.push(String(error)));
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/title-animation`);
    await page.waitForSelector('.ace-sidebar-session-title-content');
    await page.clock.install({time:new Date('2026-10-11T00:00:00Z')});
    await page.clock.pauseAt(new Date('2026-10-11T00:00:00Z'));
    const patch = values => page.evaluate(values => window.fixture.patch(values), values);
    const snapshot = () => page.evaluate(() => {
      const top = document.querySelector('.ace-session-title .ace-session-title-transition');
      const side = document.querySelector('aside .ace-session-title-transition');
      const read = element => ({
        typing:element.dataset.titleTyping,
        full:element.querySelector('.ace-session-title-complete').textContent,
        visible:element.querySelector('.ace-session-title-typing')?.textContent || element.textContent,
        fullOpacity:getComputedStyle(element.querySelector('.ace-session-title-complete')).opacity,
      });
      return {top:read(top), side:read(side),
        cursors:document.querySelectorAll('.ace-session-title-cursor').length,
        workspaceX:document.querySelector('[data-session-workspace-label]').getBoundingClientRect().x,
        marquee:getComputedStyle(document.querySelector('.ace-sidebar-session-title-content')).animationName,
        overflow:document.documentElement.scrollWidth > innerWidth,
      };
    });
    assert.equal((await snapshot()).cursors, 0);
    const title = '会话标题更新时同步呈现快速打字效果';
    await patch({title});
    let frame = await snapshot();
    assert.deepEqual(frame.top, frame.side);
    assert.equal(frame.top.visible, '会');
    assert.equal(frame.cursors, 2);
    const cursorStyles = await page.locator('.ace-session-title-cursor').evaluateAll(elements => elements.map(el => {
      const style = getComputedStyle(el);
      return {color:style.backgroundColor, width:el.getBoundingClientRect().width, height:el.getBoundingClientRect().height};
    }));
    for (const cursor of cursorStyles) {
      assert.equal(cursor.color, 'rgb(0, 0, 0)');
      assert.ok(cursor.width >= 9 && Math.abs(cursor.width - cursor.height) < 1, JSON.stringify(cursor));
    }
    assert.equal(frame.top.fullOpacity, '0');
    assert.equal(frame.side.fullOpacity, '0');
    const x = frame.workspaceX;
    await page.clock.runFor(160);
    frame = await snapshot();
    assert.deepEqual(frame.top, frame.side);
    assert.ok(frame.top.visible.length > 1 && frame.top.visible.length < title.length);
    assert.equal(frame.workspaceX, x);
    await page.locator('aside').hover();
    assert.equal((await snapshot()).marquee, 'none');
    await page.screenshot({path:path.join(output,'desktop-typing.png')});
    await patch({title});
    assert.equal((await snapshot()).top.visible, frame.top.visible);
    await page.clock.runFor(500);
    frame = await snapshot();
    assert.equal(frame.top.visible, title);
    assert.equal(frame.cursors, 0);
    assert.equal(frame.workspaceX, x);
    assert.equal(frame.marquee, 'ace-sidebar-session-title-marquee');
    await page.screenshot({path:path.join(output,'desktop-complete.png')});

    await patch({title:'连续更新标题甲'});
    await page.clock.runFor(70);
    await patch({title:'连续更新标题乙'});
    frame = await snapshot();
    assert.deepEqual(frame.top, frame.side);
    assert.equal(frame.top.full, '连续更新标题乙');
    assert.equal(frame.top.visible, '连');
    await patch({id:'two', title:'打开已有会话'});
    assert.equal((await snapshot()).cursors, 0);
    await page.clock.runFor(700);
    assert.equal((await snapshot()).top.visible, '打开已有会话');

    await patch({id:'three', title:'加载中', ready:false});
    await patch({title:'历史加载后的完整标题', ready:true});
    assert.equal((await snapshot()).top.typing, 'false');
    await page.clock.runFor(700);
    await patch({portal:true, title:'顶部窗口栏同样同步打字'});
    await page.clock.runFor(100);
    frame = await snapshot();
    assert.deepEqual(frame.top, frame.side);
    assert.equal(await page.locator('#title-target .ace-session-title-transition').count(), 1);
    await patch({renaming:true});
    assert.equal(await page.locator('[data-session-title-rename-input]').inputValue(), '顶部窗口栏同样同步打字');
    await page.locator('[data-session-title-rename-input]').fill('手动修改后的会话标题');
    await page.locator('[data-session-title-rename-input]').press('Enter');
    await page.clock.runFor(50);
    frame = await snapshot();
    assert.deepEqual(frame.top, frame.side);
    assert.equal(frame.top.full, '手动修改后的会话标题');
    assert.equal(frame.cursors, 2);
    await page.clock.runFor(700);

    await page.setViewportSize({width:390,height:500});
    await page.evaluate(() => { document.documentElement.dataset.theme = 'dark'; });
    await patch({title:'窄屏上的较长会话标题也能保持布局稳定且正确截断'});
    await page.clock.runFor(100);
    frame = await snapshot();
    assert.deepEqual(frame.top, frame.side);
    assert.equal(frame.overflow, false);
    await page.evaluate(() => document.getAnimations().filter(a => a instanceof CSSTransition).forEach(a => a.finish()));
    await page.screenshot({path:path.join(output,'mobile-dark-typing.png')});
    await page.emulateMedia({reducedMotion:'reduce'});
    await page.clock.runFor(20);
    assert.equal((await snapshot()).cursors, 2);
    await page.clock.runFor(220);
    assert.equal((await snapshot()).cursors, 0);
    await patch({title:'减少动态效果时仍能看到标题逐字更新'});
    await page.clock.runFor(100);
    frame = await snapshot();
    assert.deepEqual(frame.top, frame.side);
    assert.ok(frame.top.visible.length > 1 && frame.top.visible.length < frame.top.full.length);
    assert.equal(frame.top.fullOpacity, '0');
    assert.equal(frame.cursors, 2);
    assert.equal(await page.locator('.ace-session-title-typing').first().isVisible(), true);
    await page.screenshot({path:path.join(output,'mobile-dark-reduced.png')});
    await page.clock.runFor(230);
    frame = await snapshot();
    assert.equal(frame.top.visible, frame.top.full);
    assert.equal(frame.cursors, 0);
    assert.deepEqual(errors, []);
    const syncPage = await browser.newPage({viewport:{width:1000,height:400}});
    syncPage.on('pageerror', error => errors.push(String(error)));
    await syncPage.route('**/diagnostics/**', route => route.fulfill({json:{ok:true}}));
    await syncPage.goto(`http://127.0.0.1:${server.httpServer.address().port}/title-sync`);
    await syncPage.waitForFunction(() => window.fixture?.count() === 1);
    await syncPage.evaluate(() => fixture.resolve(0, {messages:[],events:[],title:'原标题',busy:true}));
    await syncPage.waitForFunction(() => fixture.count() === 2);
    await syncPage.clock.install({time:new Date('2026-10-12T00:00:00Z')});
    await syncPage.clock.pauseAt(new Date('2026-10-12T00:00:00Z'));
    await syncPage.evaluate(() => {
      fixture.emit('token', 20, {text:'reply'});
      fixture.emit('session_updated', 19, {title:'不用切换会话顶部就能更新',title_source:'generated'});
    });
    await syncPage.clock.runFor(100);
    const syncTitles = await syncPage.locator('.ace-session-title-typing').allTextContents();
    assert.equal(syncTitles.length, 2);
    assert.equal(syncTitles[0], syncTitles[1]);
    await syncPage.evaluate(() => fixture.resolve(1, {messages:[],events:[],title:'原标题',busy:false}));
    await syncPage.clock.runFor(700);
    assert.deepEqual(await syncPage.locator('.ace-session-title-complete').allTextContents(),
      ['不用切换会话顶部就能更新', '不用切换会话顶部就能更新']);
    assert.equal(await syncPage.locator('.ace-session-title-cursor').count(), 0);
    await syncPage.screenshot({path:path.join(output,'title-sync-after-stale-history.png')});
    await syncPage.close();
    assert.deepEqual(errors, []);
    console.log(JSON.stringify({result:'passed', output, scenarios:[
      'synchronized progress', 'immediate cursor removal', 'stable layout', 'hover marquee pause/resume',
      'duplicate updates', 'interruption', 'session switch', 'history load', 'portal title',
      'rename input', 'mobile dark theme', 'live reduced motion', 'black square cursor', 'real transcript title ordering and stale history',
    ]}));
  }
} finally {
  await browser?.close();
  await server.close();
}
