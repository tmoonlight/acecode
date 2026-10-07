import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-office-preferences-'));
const html = await fs.readFile(new URL('../../assets/desktop_pet/agent_office_pet.html', import.meta.url), 'utf8');
const fixture = `
import React,{useState} from 'react';
import {createRoot} from 'react-dom/client';
import {SidebarQuickMenu} from '/src/components/SidebarQuickMenu.jsx';
import {VirtualOfficeSettings} from '/src/components/VirtualOfficeSettings.jsx';
import {VirtualOfficeWelcome} from '/src/components/VirtualOfficeWelcome.jsx';
import {useDesktopOffice} from '/src/lib/useDesktopOffice.js';
import {api} from '/src/lib/api.js';
import {connection} from '/src/lib/connection.js';
import '/src/styles/globals.css';
let prefs={ok:true,available:true,enabled:false,welcomePending:true};
window.snapshotRequests=0;window.published=[];window.previewRequests=0;window.retained=new Set();
window.aceDesktop_getOfficePreferences=async()=>prefs;
window.aceDesktop_setOfficeEnabled=async enabled=>(prefs={...prefs,enabled,welcomePending:false});
window.aceDesktop_claimOfficeWelcome=async()=>{const show=prefs.welcomePending;prefs={...prefs,welcomePending:false};return {...prefs,show};};
window.aceDesktop_getOfficeState=async()=>({follow:true});
window.aceDesktop_updateOffice=async value=>{window.published.push(JSON.parse(value));return true;};
window.aceDesktop_getOfficePreview=async()=>{window.previewRequests++;return ${JSON.stringify(html)};};
window.nativeClose=()=>{prefs={...prefs,enabled:false};window.dispatchEvent(new CustomEvent('ace-desktop-office-preferences',{detail:prefs}));};
Object.assign(connection,{retainSession:id=>window.retained.add(id),releaseSession:id=>window.retained.delete(id)});
Object.assign(api,{getDesktopOffice:async()=>{window.snapshotRequests++;const s={id:'live',title:'当前工作',active:true,busy:true,workspace_hash:'w'};return {selected:s,agents:[s],offices:[s],complete:true};},getSessionModel:async()=>({})});
function Fixture(){
 const office=useDesktopOffice({sessionId:'live',workspaceHash:'w'});
 const [welcome,setWelcome]=useState(false);
 window.office=office;
 return <main className="h-screen p-5 text-fg bg-bg">
  <div className="max-w-[640px] mx-auto"><VirtualOfficeSettings office={office}/>
  <button id="invite" onClick={async()=>{const r=await office.claimWelcome();if(r.show)setWelcome(true);}}>Preview invitation</button></div>
  <div className="absolute bottom-4 left-4"><SidebarQuickMenu sidebarWidth={270} office={office}/></div>
  {welcome&&<VirtualOfficeWelcome office={office} onClose={()=>setWelcome(false)}/>}
 </main>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const virtual = path.join(web, '__office-preferences-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web, server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{ name: 'office-preferences-fixture', enforce: 'pre',
    resolveId(id) { return id === '/__office-preferences-fixture.jsx' ? virtual : undefined; },
    load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
    configureServer(vite) { vite.middlewares.use(async (req, res, next) => {
      if (req.url !== '/office-fixture') return next();
      res.setHeader('Content-Type', 'text/html');
      res.end(await vite.transformIndexHtml('/office-fixture', '<!doctype html><html lang="zh-CN" data-theme="light"><meta name="viewport" content="width=device-width, initial-scale=1"><body><div id="root"></div><script type="module" src="/__office-preferences-fixture.jsx"></script></body></html>'));
    }); },
  }],
});
let browser;
const errors = [], external = [];
try {
  await server.listen();
  browser = await chromium.launch({ channel: process.env.ACE_BROWSER_CHANNEL || 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1100, height: 800 } });
  page.on('pageerror', e => errors.push(e.message));
  page.on('request', r => { if (/^https?:/.test(r.url()) && !r.url().includes('127.0.0.1:')) external.push(r.url()); });
  const url = `http://127.0.0.1:${server.httpServer.address().port}/office-fixture`;
  await page.goto(url);
  await page.waitForFunction(() => window.office?.ready);
  assert.equal(await page.evaluate(() => snapshotRequests), 0, 'disabled office never polls');
  assert.equal(await page.evaluate(() => previewRequests), 0, 'preview is lazy');
  const toggle = page.getByRole('switch', { name: '开启虚拟办公室' });
  assert.equal(await toggle.getAttribute('aria-checked'), 'false');
  await page.getByRole('button', { name: 'ACECode 快捷菜单' }).click();
  const labels = await page.getByRole('menuitem').allTextContents();
  assert.ok(labels.indexOf('显示虚拟办公室') === labels.indexOf('设置') + 1);
  assert.ok(labels.indexOf('外观') === labels.indexOf('显示虚拟办公室') + 1);
  assert.equal(await page.getByRole('menuitem', { name: '显示虚拟办公室' }).locator('svg').count(), 1);
  await page.screenshot({ path: path.join(output, 'menu.png') });
  await page.keyboard.press('Escape');
  await page.locator('#invite').click();
  await page.getByRole('dialog').waitFor();
  const frame = await page.locator('iframe').elementHandle().then(e => e.contentFrame());
  await frame.waitForFunction(() => window.AgentOffice?.agents.length === 3);
  assert.equal(await page.locator('iframe').getAttribute('sandbox'), 'allow-scripts');
  assert.equal(await frame.evaluate(() => !!window.aceDesktop_updateOffice), false);
  assert.equal(await page.evaluate(() => snapshotRequests), 0, 'demo does not enable live subscriptions');
  await page.mouse.click(10, 10);
  assert.equal(await page.getByRole('dialog').count(), 1, 'backdrop does not dismiss');
  for (const theme of ['light', 'dark']) for (const width of [1100, 390]) {
    await page.setViewportSize({ width, height: 800 });
    await page.evaluate(t => { document.documentElement.dataset.theme = t; }, theme);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    await page.screenshot({ path: path.join(output, `welcome-${theme}-${width}.png`) });
  }
  await page.getByRole('button', { name: '暂停动画' }).click();
  const canvas = frame.locator('#cv');
  const frozen = await canvas.screenshot();
  await page.waitForTimeout(300);
  assert.deepEqual(await canvas.screenshot(), frozen, 'pause freezes canvas rendering');
  await page.getByRole('button', { name: '播放动画' }).click();
  await page.waitForTimeout(400);
  assert.notDeepEqual(await canvas.screenshot(), frozen, 'resume animates');
  const detached = page.waitForEvent('framedetached', { predicate: value => value === frame });
  await page.getByRole('button', { name: '暂不开启', exact: true }).click();
  await detached;
  await page.locator('iframe').waitFor({ state: 'detached' });
  assert.equal(await page.locator('iframe').count(), 0, 'dismissal destroys the preview iframe');
  assert.ok(frame.isDetached(), 'the sandbox frame is detached');
  assert.equal(await toggle.getAttribute('aria-checked'), 'false');
  await page.locator('#invite').click();
  assert.equal(await page.getByRole('dialog').count(), 0, 'acknowledged invitation is not repeated');
  await toggle.click();
  await page.waitForFunction(() => window.published.at(-1)?.selected?.title === '当前工作');
  assert.equal(await page.evaluate(() => published.at(-1).selected.title), '当前工作');
  assert.equal(await page.evaluate(() => retained.has('live')), true, 'enabled office retains the live session');
  await page.evaluate(() => nativeClose());
  await page.waitForFunction(() => !office.enabled && retained.size === 0);
  const requests = await page.evaluate(() => snapshotRequests);
  await page.waitForTimeout(3200);
  assert.equal(await page.evaluate(() => snapshotRequests), requests, 'native close stops polling');
  await page.getByRole('button', { name: 'ACECode 快捷菜单' }).click();
  await page.getByRole('menuitem', { name: '显示虚拟办公室' }).click();
  await page.waitForFunction(n => snapshotRequests > n, requests);
  assert.equal(await toggle.getAttribute('aria-checked'), 'true', 'menu reopens office and updates setting');
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.reload(); await page.waitForFunction(() => window.office?.ready);
  await page.locator('#invite').click();
  await page.getByRole('button', { name: '播放动画' }).waitFor();
  await page.getByRole('button', { name: '开启虚拟办公室', exact: true }).click();
  await page.waitForFunction(() => office.enabled && published.length > 0);
  await page.locator('iframe').waitFor({ state: 'detached' });
  assert.equal(await page.getByRole('dialog').count(), 0);
  assert.equal(await page.locator('iframe').count(), 0);
  assert.deepEqual(errors, []);
  assert.deepEqual(external, []);
  console.log(JSON.stringify({ ok: true, coverage: 'opt-in, menu ordering, close/reopen, polling teardown, sandbox preview, light/dark, narrow viewport, pause, reduced motion, once-only claim', output }));
} finally { await browser?.close(); await server.close(); }
