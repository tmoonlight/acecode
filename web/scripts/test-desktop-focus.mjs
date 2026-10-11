import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const fixture = `
import React, {useEffect, useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {RichComposer} from '/src/components/RichComposer.jsx';
import {bindDesktopComposerAutoFocus} from '/src/lib/composerCaretRestore.js';
import '/src/styles/globals.css';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const events = [];
for (const type of ['focus','blur','acecode:desktop-window-focus']) {
  window.addEventListener(type, () => events.push([type, document.hasFocus(), document.activeElement?.outerHTML?.slice(0,100)]));
}
function App() {
  const editor = useRef(null);
  const [value, setValue] = useState('draft');
  const [enabled, setEnabled] = useState(true);
  useEffect(() => {
    window.editor = editor;
    window.setChatVisible = setEnabled;
    return bindDesktopComposerAutoFocus({enabled,
      onFocus: canFocus => editor.current?.focus({canFocus})});
  }, [enabled]);
  useEffect(() => {window.text = value;}, [value]);
  return <main style={{padding:40}}>
    <div data-ace-focus-region="terminal"><input id="terminal" aria-label="Terminal" /></div>
    <button id="other">Other control</button>
    {enabled && <RichComposer ref={editor} value={value} onChange={setValue} aria-label="Chat" />}
  </main>;
}
createRoot(document.getElementById('root')).render(<App/>);
window.activate = () => window.dispatchEvent(new Event('acecode:desktop-window-focus'));
if (window.aceFocusTest) {
  (async () => {
    const checks = [];
    try {
      await sleep(600); await aceFocusTest('show'); await sleep(400);
      for (const [name, operations] of [
        ['window switch', ['away', 'return']],
        ['minimize/restore', ['minimize', 'restore']],
        ['maximize', ['maximize']],
        ['unmaximize', ['unmaximize']],
        ['hide/show', ['hide', 'show']],
      ]) {
        document.querySelector('#terminal').focus();
        for (const op of operations) {await aceFocusTest(op); await sleep(250);}
        const before = window.text;
        if (document.activeElement?.getAttribute('data-ace-rich-composer') !== 'true')
          throw new Error(name + ': composer not active ' + JSON.stringify({focused:document.hasFocus(),active:document.activeElement?.outerHTML?.slice(0,150),events}));
        await aceFocusTest('type'); await sleep(100);
        if (window.text !== before + 'Z') throw new Error(name + ': real keystroke missing: ' + window.text);
        checks.push(name + ': real keyboard input passed');
      }
      editor.current.setSelectionRange(1, 3);
      await sleep(80); await aceFocusTest('away'); await sleep(150);
      const background = await aceDesktop_focusActiveWebview();
      if (background.ok !== false) throw new Error('background window accepted focus');
      await aceFocusTest('return'); await sleep(250);
      if (window.getSelection().toString() !== 'ra') throw new Error('selection lost');
      checks.push('background refusal and selection preservation passed');
      const modal = document.createElement('div'); modal.setAttribute('role','dialog');
      modal.innerHTML = '<input id="modal-input">'; document.body.append(modal);
      modal.firstChild.focus(); await aceFocusTest('away'); await sleep(150);
      await aceFocusTest('return'); await sleep(250);
      if (document.activeElement !== modal.firstChild) throw new Error('modal focus stolen');
      checks.push('modal protection passed');
      await aceFocusTest('done', true, checks);
    } catch (error) {await aceFocusTest('done', false, {checks, error: String(error)});}
  })();
}
`;
const virtual = path.join(web, '__desktop-focus-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web, logLevel: 'error',
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{ name: 'desktop-focus-fixture', enforce: 'pre',
    resolveId(id) { return id === '/__desktop-focus-fixture.jsx' ? virtual : undefined; },
    load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
    configureServer(vite) { vite.middlewares.use(async (req, res, next) => {
      if (req.url !== '/focus-fixture') return next();
      res.setHeader('Content-Type', 'text/html');
      res.end(await vite.transformIndexHtml('/focus-fixture', '<!doctype html><html><body><div id="root"></div><script type="module" src="/__desktop-focus-fixture.jsx"></script></body></html>'));
    }); },
  }],
});
try {
  await server.listen();
  const url = `http://127.0.0.1:${server.httpServer.address().port}/focus-fixture`;
  if (process.env.ACE_FOCUS_NATIVE_HOST) {
    const profile = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-focus-webview-'));
    await new Promise((resolve, reject) => {
      const child = spawn(process.env.ACE_FOCUS_NATIVE_HOST, [url], { windowsHide: true,
        env: { ...process.env, WEBVIEW2_USER_DATA_FOLDER: profile }, stdio: 'inherit' });
      const timer = setTimeout(() => {child.kill(); reject(new Error('Native focus test timed out'));}, 60000);
      child.on('error', error => {clearTimeout(timer); reject(error);});
      child.on('exit', code => {
        clearTimeout(timer);
        if (code === 77) { process.exitCode = 77; resolve(); }
        else if (code === 0) resolve();
        else reject(new Error(`Native focus test exit ${code}`));
      });
    });
  } else {
    const playwright = await import(process.env.ACE_PLAYWRIGHT_MODULE
      ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
    for (const engine of ['chromium', 'webkit']) {
      const browser = await playwright[engine].launch({headless: true});
      try {
        const page = await browser.newPage();
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(url);
        const editor = page.locator('[data-ace-rich-composer="true"]');
        await editor.click();
        await page.keyboard.press('End');
        await page.keyboard.type(' test');
        await page.waitForTimeout(100);
        await page.evaluate(() => {
          window.nativeFocusCalls = 0;
          window.aceDesktop_focusActiveWebview = async () => {
            window.nativeFocusCalls += 1;
            window.dispatchEvent(new Event('focus'));
            return {ok: true};
          };
        });
        await page.locator('#terminal').click();
        await page.evaluate(() => window.activate());
        await page.waitForTimeout(150);
        await page.keyboard.type('Z');
        assert.equal(await editor.innerText(), 'draft testZ');
        assert.equal(await page.evaluate(() => window.nativeFocusCalls), 0, 'focused pages do not repeat the native focus handoff');
        await page.evaluate(() => { delete window.aceDesktop_focusActiveWebview; });
        await page.evaluate(() => window.editor.current.setSelectionRange(1, 3));
        await page.waitForTimeout(40);
        await page.locator('#other').focus();
        await page.evaluate(() => window.activate());
        await page.waitForTimeout(150);
        assert.equal(await page.evaluate(() => window.getSelection().toString()), 'ra');
        await page.keyboard.type('X');
        assert.equal(await editor.innerText(), 'dXft testZ');
        await page.evaluate(() => {
          const modal = document.createElement('div'); modal.setAttribute('role','dialog');
          modal.innerHTML = '<input id="modal-input">'; document.body.append(modal);
          modal.firstChild.focus(); window.activate();
        });
        await page.waitForTimeout(150);
        await page.keyboard.type('modal');
        assert.equal(await page.locator('#modal-input').inputValue(), 'modal');
        await page.evaluate(() => {
          document.querySelector('[role="dialog"]').remove(); window.activate();
          window.setChatVisible(false);
        });
        await page.waitForTimeout(150);
        assert.equal(await editor.count(), 0);
        assert.deepEqual(errors, []);
        console.log(`[pass] ${engine}: keyboard input, terminal handoff, selection, modal, unmount`);
      } finally { await browser.close(); }
    }
  }
} finally { await server.close(); }
