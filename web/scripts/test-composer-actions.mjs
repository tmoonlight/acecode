import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

// Run from web: node scripts/test-composer-actions.mjs
// Playwright is optional; ACE_PLAYWRIGHT_MODULE may point to its index.mjs.
// ACE_CHROMIUM_EXECUTABLE selects an installed Chromium. Results and screenshots
// go to ACE_COMPOSER_ACTION_OUTPUT, or a directory in the system temp folder.
const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = process.env.ACE_COMPOSER_ACTION_OUTPUT || path.join(os.tmpdir(), 'acecode-composer-actions');
const virtual = path.join(web, '__composer-actions.jsx').replaceAll('\\', '/');
const fixture = `
import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import {InputBar} from '/src/components/InputBar.jsx';
import {composerContentFromText} from '/src/lib/composerContent.js';
import {i18n} from '/src/i18n/index.js';
import '/src/styles/globals.css';
const params = new URLSearchParams(location.search);
document.documentElement.dataset.theme = params.get('theme') || 'light';
await i18n.changeLanguage(params.get('locale') || 'zh-CN');
const flag = key => params.get(key) === 'true';
const busy = params.get('busy') !== 'false';
const attachments = flag('attachment') ? [{id:'fixture-file',key:'fixture-file',name:'notes.txt',kind:'file'}] : [];
const events = [];
function Fixture() {
  const [value,setValue] = useState(params.get('text') || '');
  const [content,setContent] = useState(composerContentFromText(params.get('text') || '', attachments));
  const [stopping,setStopping] = useState(flag('stopping'));
  window.fixture = {value,events};
  return <main style={{maxWidth:720,margin:'60px auto',padding:16}}>
    <div id="accent-probe" style={{background:'var(--ace-accent)'}} />
    <InputBar mainComposer busy={busy} stopping={stopping}
      disabled={flag('disabled')} submitting={flag('submitting')}
      canRetryLastUserMessage={flag('retry')} queuePaused={flag('paused')}
      value={value} onChange={setValue} composerContent={content} onComposerContentChange={setContent}
      attachments={attachments}
      onSubmit={text=>{events.push({action:busy?'queue':'send',text});setValue('');setContent(composerContentFromText(''));}}
      onAbort={()=>{events.push({action:'stop'});setStopping(true);}}
      onResumeQueue={()=>events.push({action:'resume'})} />
  </main>;
}
createRoot(document.getElementById('root')).render(<Fixture/>);
`;
const html = `<html><body><div id="root"></div><script type="module">
import RefreshRuntime from '/@react-refresh';
RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;
window.__vite_plugin_react_preamble_installed__=true;
</script><script type="module" src="/__composer-actions.jsx"></script></body></html>`;
const server = await createServer({
  root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  plugins: [{
    name: 'composer-actions-fixture', enforce: 'pre',
    resolveId(id) { if (id === '/__composer-actions.jsx') return virtual; },
    load(id) { if (id.replaceAll('\\', '/') === virtual) return fixture; },
    configureServer(vite) {
      vite.middlewares.use((req, res, next) => {
        if (req.url?.split('?')[0] !== '/__composer-actions') return next();
        res.setHeader('Content-Type', 'text/html'); res.end(html);
      });
    },
  }],
  server: { host: '127.0.0.1', port: Number(process.env.ACE_COMPOSER_ACTION_PORT || 5192), strictPort: true, hmr: false },
});
let browser;
const results = [];
const errors = [];
try {
  await mkdir(output, { recursive: true });
  await server.listen();
  browser = await chromium.launch({ headless: true,
    ...(process.env.ACE_CHROMIUM_EXECUTABLE ? { executablePath: process.env.ACE_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage({ viewport: { width: 1100, height: 600 } });
  page.on('pageerror', error => errors.push(error.message));
  const editor = page.locator('[data-ace-rich-composer]');
  const button = page.locator('[data-composer-action]');
  const settle = () => page.evaluate(() => new Promise(resolve => setTimeout(() => requestAnimationFrame(resolve), 150)));
  async function reset(params = {}) {
    await page.goto(server.resolvedUrls.local[0] + '__composer-actions?' + new URLSearchParams(params));
    await editor.waitFor(); await page.evaluate(() => document.fonts.ready); await settle();
  }
  async function state() {
    await settle();
    return button.evaluate(el => ({
      ...window.fixture, count: document.querySelectorAll('[data-composer-action]').length,
      mode: el.dataset.composerAction, label: el.getAttribute('aria-label'), disabled: el.disabled,
      icon: el.querySelector('[data-icon-name]')?.dataset.iconName,
      background: getComputedStyle(el).backgroundColor,
      accent: getComputedStyle(document.getElementById('accent-probe')).backgroundColor,
      opacity: getComputedStyle(el).opacity, rect: (()=>{const r=el.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height};})(),
    }));
  }
  function expectMode(actual, mode, icon, disabled = false) {
    assert.equal(actual.count, 1); assert.equal(actual.mode, mode); assert.equal(actual.icon, icon);
    assert.equal(actual.disabled, disabled); assert.equal(actual.background, actual.accent);
    assert.equal(actual.rect.width, 28); assert.equal(actual.rect.height, 28);
  }
  async function run(name, fn) {
    try { await fn(); results.push({ name, passed: true }); console.log('[pass] ' + name); }
    catch (error) { results.push({ name, passed: false, error: error.message }); throw error; }
  }
  await run('empty and whitespace keep stop; empty Enter does nothing; stop cannot repeat', async () => {
    await reset(); expectMode(await state(), 'stop', 'StopFilled');
    await editor.click(); await page.keyboard.insertText('   '); await page.keyboard.press('Enter');
    const empty = await state(); expectMode(empty, 'stop', 'StopFilled'); assert.deepEqual(empty.events, []);
    await button.click(); const stopping = await state(); expectMode(stopping, 'stop', 'StopFilled', true);
    assert.equal(stopping.label, '正在停止…'); assert.deepEqual(stopping.events, [{ action: 'stop' }]);
    await page.keyboard.press('Enter'); assert.deepEqual((await state()).events, [{ action: 'stop' }]);
  });
  await run('same button changes to queue; click and Enter queue then return to stop', async () => {
    await reset(); const initial = await state();
    await editor.click(); await page.keyboard.insertText('next step');
    const queued = await state(); expectMode(queued, 'queue', 'Queue'); assert.equal(queued.label, '排队');
    assert.deepEqual(queued.rect, initial.rect);
    await button.click(); expectMode(await state(), 'stop', 'StopFilled');
    await editor.click(); await page.keyboard.insertText('another step'); await settle(); await page.keyboard.press('Enter');
    const sent = await state(); expectMode(sent, 'stop', 'StopFilled');
    assert.deepEqual(sent.events, [{ action: 'queue', text: 'next step' }, { action: 'queue', text: 'another step' }]);
  });
  await run('clearing content returns to stop; Shift+Enter inserts a newline', async () => {
    await reset({ text: 'next step' });
    await editor.click(); await page.keyboard.press('ControlOrMeta+A'); await page.keyboard.press('Backspace');
    expectMode(await state(), 'stop', 'StopFilled');
    await page.keyboard.insertText('first'); await page.keyboard.press('Shift+Enter'); await page.keyboard.insertText('second');
    const multiline = await state(); expectMode(multiline, 'queue', 'Queue');
    assert.equal(multiline.value, 'first\nsecond'); assert.deepEqual(multiline.events, []);
  });
  await run('attachment-only draft queues through Enter', async () => {
    await reset({ attachment: true }); expectMode(await state(), 'queue', 'Queue');
    await editor.click(); await page.keyboard.press('Enter');
    const sent = await state(); assert.equal(sent.events[0]?.action, 'queue'); expectMode(sent, 'stop', 'StopFilled');
  });
  await run('submission and blocking guards disable queue while stop stays available', async () => {
    for (const blocker of ['disabled', 'submitting']) {
      await reset({ text: 'next step', [blocker]: true }); expectMode(await state(), 'queue', 'Queue', true);
      await reset({ [blocker]: true }); expectMode(await state(), 'stop', 'StopFilled');
    }
    await reset({ stopping: true }); expectMode(await state(), 'stop', 'StopFilled', true);
  });
  await run('idle send, empty retry, and paused queue resume retain their actions', async () => {
    await reset({ busy: false }); expectMode(await state(), 'send', 'Send', true);
    await reset({ busy: false, text: 'hello' }); await button.click();
    assert.deepEqual((await state()).events, [{ action: 'send', text: 'hello' }]);
    await reset({ busy: false, retry: true }); expectMode(await state(), 'send', 'Send');
    await button.click(); assert.deepEqual((await state()).events, [{ action: 'send', text: '' }]);
    await reset({ busy: false, paused: true }); expectMode(await state(), 'resume', 'Run');
    await editor.click(); await page.keyboard.press('Enter'); assert.deepEqual((await state()).events, [{ action: 'resume' }]);
  });
  await run('light/dark and narrow/wide retain accent background and stable geometry', async () => {
    for (const theme of ['light', 'dark']) for (const width of [1100, 390]) {
      await page.setViewportSize({ width, height: 600 });
      for (const mode of ['stop', 'queue']) {
        await reset({ theme, text: mode === 'queue' ? 'next step' : '' });
        const actual = await state(); expectMode(actual, mode, mode === 'stop' ? 'StopFilled' : 'Queue');
        assert.ok(actual.rect.x >= 0 && actual.rect.x + actual.rect.width <= width);
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
        await page.locator('main').screenshot({ path: path.join(output, `${theme}-${width}-${mode}.png`) });
      }
    }
  });
  await run('English actions translate and keyboard focus is visible', async () => {
    await reset({ locale: 'en-US' }); assert.equal((await state()).label, 'Stop');
    await editor.click();
    for (let index = 0; index < 12; index += 1) {
      await page.keyboard.press('Tab');
      if (await button.evaluate(el => el === document.activeElement)) break;
    }
    const focus = await button.evaluate(el => ({ focused: el === document.activeElement, width: getComputedStyle(el).outlineWidth }));
    assert.equal(focus.focused, true); assert.equal(focus.width, '2px');
    await reset({ locale: 'en-US', text: 'next step' }); assert.equal((await state()).label, 'queue');
  });
  await run('no browser runtime errors', async () => assert.deepEqual(errors, []));
} finally {
  await writeFile(path.join(output, 'results.json'), JSON.stringify({ results, errors }, null, 2) + '\n');
  await browser?.close(); await server.close();
}
