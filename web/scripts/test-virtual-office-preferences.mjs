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
const fixture = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {SidebarQuickMenu} from '/src/components/SidebarQuickMenu.jsx';
import {VirtualOfficeSettings} from '/src/components/VirtualOfficeSettings.jsx';
import {useDesktopOffice} from '/src/lib/useDesktopOffice.js';
import {api} from '/src/lib/api.js';
import {connection} from '/src/lib/connection.js';
import '/src/styles/globals.css';
let prefs={ok:true,available:true,enabled:false};
window.snapshotRequests=0;window.published=[];window.retained=new Set();
window.aceDesktop_getOfficePreferences=async()=>prefs;
window.aceDesktop_setOfficeEnabled=async enabled=>(prefs={...prefs,enabled});
window.aceDesktop_setPetKind=async petKind=>(prefs={...prefs,petKind});
window.aceDesktop_getOfficeState=async()=>({follow:true});
window.aceDesktop_updateOffice=async value=>{window.published.push(JSON.parse(value));return true;};
window.nativeClose=()=>{prefs={...prefs,enabled:false};window.dispatchEvent(new CustomEvent('ace-desktop-office-preferences',{detail:prefs}));};
Object.assign(connection,{retainSession:id=>window.retained.add(id),releaseSession:id=>window.retained.delete(id)});
Object.assign(api,{getDesktopOffice:async()=>{window.snapshotRequests++;const s={id:'live',title:'当前工作',active:true,busy:true,workspace_hash:'w'};return {selected:s,agents:[s],offices:[s],complete:true};},getSessionModel:async()=>({})});
function Fixture(){
 const office=useDesktopOffice({sessionId:'live',workspaceHash:'w'});
 window.office=office;
 return <main className="h-screen p-5 text-fg bg-bg">
  <div className="max-w-[640px] mx-auto"><VirtualOfficeSettings office={office}/></div>
  <div className="absolute bottom-4 left-4"><SidebarQuickMenu sidebarWidth={270} office={office}/></div>
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
  assert.equal(await page.getByRole('dialog').count(), 0);
  assert.equal(await page.locator('iframe').count(), 0);
  const toggle = page.getByRole('switch', { name: '开启桌面宠物' });
  assert.equal(await toggle.getAttribute('aria-checked'), 'false');
  assert.deepEqual(await page.getByRole('radio').locator('div.text-fg').allTextContents(), ['虚拟办公室', '大眼回形针', 'rover', '鲸鱼娘']);
  for (const [label, kind] of [['大眼回形针', 'clippy'], ['rover', 'rover'], ['鲸鱼娘', 'whale'], ['虚拟办公室', 'office']]) {
    await page.getByRole('radio', { name: label }).click();
    await page.waitForFunction(value => office.petKind === value, kind);
    assert.equal(await toggle.getAttribute('aria-checked'), 'false', 'selection does not enable the pet');
  }
  for (const [theme, width] of [['light', 1100], ['dark', 1100], ['light', 390]]) {
    await page.setViewportSize({ width, height: 800 });
    await page.evaluate(value => { document.documentElement.dataset.theme = value; }, theme);
    await page.waitForTimeout(400);
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    await page.screenshot({ path: path.join(output, `pets-${theme}-${width}.png`) });
  }
  await page.setViewportSize({ width: 1100, height: 800 });
  await page.getByRole('button', { name: 'ACECode 快捷菜单' }).click();
  // Read labels separately from the shortcut badges added to menu items.
  const labels = await page.getByRole('menuitem').locator('span.truncate').allTextContents();
  assert.ok(labels.indexOf('显示桌面宠物') === labels.indexOf('设置') + 1);
  assert.ok(labels.indexOf('外观') === labels.indexOf('显示桌面宠物') + 1);
  assert.equal(await page.getByRole('menuitem', { name: '显示桌面宠物' }).locator('svg').count(), 1);
  await page.screenshot({ path: path.join(output, 'menu.png') });
  await page.keyboard.press('Escape');
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
  await page.getByRole('menuitem', { name: '显示桌面宠物' }).click();
  await page.waitForFunction(n => snapshotRequests > n, requests);
  assert.equal(await toggle.getAttribute('aria-checked'), 'true', 'menu reopens office and updates setting');
  await page.reload(); await page.waitForFunction(() => window.office?.ready);
  assert.equal(await page.getByRole('dialog').count(), 0);
  assert.equal(await page.locator('iframe').count(), 0);
  assert.deepEqual(errors, []);
  assert.deepEqual(external, []);

  // Exercise the production App with fresh-install and upgrade flags from an older shell.
  for (const welcomePending of [true, false]) {
    const startup = await browser.newPage({ viewport: { width: 1100, height: 800 } });
    startup.on('pageerror', e => errors.push(e.message));
    await startup.addInitScript(welcome => {
      class FixtureWebSocket extends EventTarget {
        static OPEN = 1; static CLOSED = 3;
        constructor() { super(); this.readyState = 1; setTimeout(() => {
          this.onopen?.({}); this.dispatchEvent(new Event('open'));
        }, 0); }
        send() {} close() { this.readyState = 3; }
      }
      window.WebSocket = FixtureWebSocket;
      window.petCalls = { preferences: 0, claims: 0, previews: 0 };
      const prefs = { ok: true, available: true, enabled: false,
        welcomePending: welcome, petsPromoPending: !welcome };
      window.aceDesktop_getOfficePreferences = async () => { window.petCalls.preferences++; return prefs; };
      window.aceDesktop_claimOfficeWelcome = window.aceDesktop_claimPetsPromo = async () => {
        window.petCalls.claims++; return { ...prefs, show: true };
      };
      window.aceDesktop_getOfficePreview = async () => { window.petCalls.previews++; return '<html><head></head></html>'; };
    }, welcomePending);
    await startup.route('**/api/**', async route => {
      const { pathname } = new URL(route.request().url());
      let data = {};
      if (pathname === '/api/health') data = { status: 'ok', version: '99.0.0', cwd: 'C:/fixture' };
      else if (pathname === '/api/config/ui-preferences') data = { theme: 'light', color_theme: 'blue', font_size: 'medium' };
      else if (pathname === '/api/workspaces' || pathname.endsWith('/sessions') || pathname.endsWith('/session-sidebar')) data = [];
      else if (pathname.includes('models')) data = { models: [], profiles: [] };
      else if (pathname.includes('history')) data = { items: [], history: [] };
      else if (pathname.includes('commands')) data = { commands: [] };
      else if (pathname.includes('skills')) data = { skills: [] };
      else if (pathname.includes('experts')) data = { experts: [] };
      else if (pathname.includes('subagents')) data = { tasks: [] };
      else if (pathname.includes('loops') || pathname.includes('/pty') || pathname.includes('/themes')) data = [];
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(data) });
    });
    await startup.goto(`http://127.0.0.1:${server.httpServer.address().port}/`);
    await startup.locator('[data-tour-target="home-composer"]').waitFor();
    await startup.waitForFunction(() => window.petCalls.preferences > 0);
    await startup.waitForTimeout(500);
    assert.equal(await startup.locator('#office-welcome-title, #pets-promo-title').count(), 0);
    assert.deepEqual(await startup.evaluate(() => [petCalls.claims, petCalls.previews]), [0, 0]);
    await startup.screenshot({ path: path.join(output, welcomePending ? 'startup-new.png' : 'startup-upgrade.png') });
    await startup.close();
  }
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ ok: true, coverage: 'production App startup with fresh and upgrade flags, opt-in, menu ordering, close/reopen, polling teardown, light/dark, narrow viewport, pet selection', output }));
} finally { await browser?.close(); await server.close(); }
