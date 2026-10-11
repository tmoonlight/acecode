import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-skill-pagination-'));
const fixture = `
import React from 'react';
import {createRoot} from 'react-dom/client';
import {SettingsPage} from '/src/components/SettingsPage.jsx';
import {api} from '/src/lib/api.js';
import {i18n} from '/src/i18n/index.js';
import '/src/styles/globals.css';
window.i18n=i18n;window.globalCount=2300;window.projectCount=123;window.listCalls=0;window.toggles=[];
const enabled=new Map();
const catalog=(count,source)=>Array.from({length:count},(_,i)=>{
 const name=source+'-'+String(i).padStart(4,'0');
 return {name,source,description:i===count-1?'needle':'Skill description '+i,enabled:enabled.get(name)??i%2===0};
});
Object.assign(api,{
 listSkills:async workspace=>{window.listCalls++;return catalog(workspace?window.projectCount:window.globalCount,workspace?'project':'global');},
 listWorkspaces:async()=>[{hash:'workspace',cwd:'C:/fixture',name:'Fixture workspace'}],
 setSkillEnabled:async(name,value,workspace)=>{window.toggles.push({name,value,workspace});enabled.set(name,value);return {name,enabled:value};}
});
createRoot(document.getElementById('root')).render(<SettingsPage initialNavKey="skills" onClose={()=>{}}/>);
`;
const virtual = path.join(web, '__skill-pagination-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web, logLevel: 'error', server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{ name: 'skill-pagination-fixture', enforce: 'pre',
    resolveId(id) { return id === '/__skill-pagination-fixture.jsx' ? virtual : undefined; },
    load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
    configureServer(vite) { vite.middlewares.use(async (req, res, next) => {
      if (req.url !== '/skill-pagination-fixture') return next();
      res.setHeader('Content-Type', 'text/html');
      res.end(await vite.transformIndexHtml('/skill-pagination-fixture', '<!doctype html><html lang="zh-CN" data-theme="light"><meta name="viewport" content="width=device-width, initial-scale=1"><body><div id="root"></div><script type="module" src="/__skill-pagination-fixture.jsx"></script></body></html>'));
    }); },
  }],
});
let browser;
const errors = [];
const checks = [];
const pass = (name) => { checks.push(name); console.log('[pass]', name); };
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  page.setDefaultTimeout(20000);
  page.on('pageerror', error => errors.push(error.message));
  await page.addInitScript(() => localStorage.setItem('ace.locale', 'zh-CN'));
  const url = `http://127.0.0.1:${server.httpServer.address().port}/skill-pagination-fixture`;
  await page.goto(url);
  const globalList = page.locator('[data-skill-list="global"]');
  const projectList = page.locator('[data-skill-list="workspace"]');
  const cards = (list) => list.locator('[data-skill-card="true"]');
  const top = (list) => list.locator('[data-skill-pagination="top"]');
  const next = (list) => top(list).getByRole('button', { name: '下一页', exact: true });
  const size = page.getByRole('combobox', { name: '每页条目数' });
  const search = page.getByPlaceholder('搜索技能名称或描述...');
  await cards(globalList).nth(49).waitFor();
  assert.equal(await cards(globalList).count(), 50);
  assert.equal(await size.inputValue(), '50');
  assert.match(await top(globalList).innerText(), /2300/);
  const initialCalls = await page.evaluate(() => window.listCalls);
  pass('2,300 global skills render only the default 50 cards');

  await next(globalList).focus();
  await page.keyboard.press('Enter');
  await globalList.getByText('global-0050', { exact: true }).waitFor();
  const toggle = cards(globalList).first().getByRole('switch');
  await toggle.click();
  await page.waitForFunction(() => window.toggles.length === 1);
  assert.equal(await toggle.getAttribute('aria-checked'), 'false');
  assert.match(await cards(globalList).first().innerText(), /global-0050/);
  await page.getByText('1149 / 2300 已启用', { exact: true }).waitFor();
  pass('keyboard next page and toggle preserve page and full-catalog enabled count');

  await page.getByRole('button', { name: /Fixture workspace/ }).first().click();
  await cards(projectList).nth(49).waitFor();
  await next(projectList).click();
  await projectList.getByText('project-0050', { exact: true }).waitFor();
  assert.match(await cards(globalList).first().innerText(), /global-0050/);
  for (const pageSize of [20, 100]) {
    await size.selectOption(String(pageSize));
    await globalList.getByText('global-0000', { exact: true }).waitFor();
    assert.equal(await cards(globalList).count(), pageSize);
    assert.equal(await cards(projectList).count(), pageSize);
    assert.match(await cards(projectList).first().innerText(), /project-0000/);
  }
  await next(projectList).click();
  assert.equal(await cards(projectList).count(), 23);
  assert.equal(await cards(globalList).count(), 100);
  pass('global and workspace pages are independent; 20/100 reset both lists');

  await search.fill('needle');
  await globalList.getByText('global-2299', { exact: true }).waitFor();
  await projectList.getByText('project-0122', { exact: true }).waitFor();
  assert.equal(await cards(globalList).count(), 1);
  assert.equal(await cards(projectList).count(), 1);
  await search.fill('not-present');
  await page.getByText('无匹配的全局技能', { exact: true }).waitFor();
  assert.equal(await page.locator('[data-skill-card="true"]').count(), 0);
  await search.fill('');
  await globalList.getByText('global-0000', { exact: true }).waitFor();
  assert.equal(await page.evaluate(() => window.listCalls), initialCalls);
  pass('search finds off-page skills in both catalogs and page changes do not rescan');

  await size.selectOption('50');
  await next(globalList).click();
  await next(globalList).click();
  await next(projectList).click();
  await page.evaluate(() => { window.globalCount = 70; window.projectCount = 60; });
  await page.getByTitle('刷新技能列表', { exact: true }).click();
  await page.waitForFunction(() => document.querySelectorAll('[data-skill-list="global"] [data-skill-card]').length === 20
    && document.querySelectorAll('[data-skill-list="workspace"] [data-skill-card]').length === 10);
  assert.match(await cards(globalList).first().innerText(), /global-0050/);
  assert.match(await cards(projectList).first().innerText(), /project-0050/);
  await page.evaluate(() => { window.globalCount = 150; window.projectCount = 111; });
  await page.getByTitle('刷新技能列表', { exact: true }).click();
  await cards(globalList).nth(49).waitFor();
  assert.match(await cards(globalList).first().innerText(), /global-0050/);
  pass('refresh clamps removed pages, retains workspace page and does not rebound after growth');

  await size.selectOption('20');
  await page.reload();
  await cards(globalList).nth(19).waitFor();
  assert.equal(await size.inputValue(), '20');
  assert.equal(await cards(globalList).count(), 20);
  await page.evaluate(() => localStorage.setItem('ace.skillsPageSize', '999'));
  await page.reload();
  await cards(globalList).nth(49).waitFor();
  assert.equal(await size.inputValue(), '50');
  await globalList.locator('[data-skill-pagination="bottom"]').getByRole('button', { name: '下一页', exact: true }).click();
  await globalList.getByText('global-0050', { exact: true }).waitFor();
  assert.equal(await top(globalList).evaluate((element) => document.activeElement === element), true);
  pass('page size persists, invalid preferences fall back to 50, bottom navigation focuses the top');

  for (const [theme, width] of [['light', 1280], ['dark', 480]]) {
    await page.setViewportSize({ width, height: 900 });
    await page.evaluate(theme => { document.documentElement.dataset.theme = theme; }, theme);
    await top(globalList).scrollIntoViewIfNeeded();
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    await page.screenshot({ path: path.join(output, `pagination-${theme}-${width}.png`) });
  }
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.evaluate(() => window.i18n.changeLanguage('en-US'));
  await page.getByRole('combobox', { name: 'Items per page' }).waitFor();
  assert.match(await top(globalList).innerText(), /Page 2 \/ 46, 2300 items/);
  assert.equal(await next(globalList).count(), 0);
  await page.getByRole('button', { name: 'Next page', exact: true }).first().waitFor();
  pass('responsive light/dark and English labels');
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ ok: true, checks, output }));
} finally { await browser?.close(); await server.close(); }
