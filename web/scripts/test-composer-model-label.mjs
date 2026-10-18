import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-model-label-'));
const html = `<!doctype html><html><head><meta charset="utf-8"></head><body>
<div id="root"></div><script type="module">
import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import '/src/styles/globals.css';
import { ComposerSessionControls } from '/src/components/ComposerSessionControls.jsx';
const h = React.createElement;
function Fixture() {
  const [state, setState] = useState({ width: 850, name: 'ACEModel-gpt-6-astra', extras: false });
  window.updateFixture = patch => setState(current => ({ ...current, ...patch }));
  const option = { name: state.name, provider: 'openai', model: 'backend-model', models_dev_provider_id: 'acemodel' };
  return h('div', { id: 'fixture', style: { width: state.width, margin: 24, padding: 12,
    border: '1px solid var(--ace-border)', borderRadius: 12, background: 'var(--ace-bg)' } },
    h('div', { style: { height: 70 } }),
    h(ComposerSessionControls, { modelOptions: [option], selectedModelName: state.name,
      model: state.name, onModelChange() {}, onRefreshModels() {},
      goalMode: state.extras, swarmMode: state.extras ? 'mesh' : 'off',
      expertName: state.extras ? 'A detailed expert name' : '',
      permissionMode: 'default', reasoningOptions: { label: 'High', items: [] },
      addControl: h('button', { style: { width: 28 }, 'aria-label': 'Add context' }, '+'),
      actions: h('button', { style: { width: 32 }, 'aria-label': 'Send' }, 'Go') }));
}
createRoot(document.getElementById('root')).render(h(Fixture));
</script></body></html>`;
const server = await createServer({ root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  cacheDir: path.join(output, 'node_modules/.vite'),
  optimizeDeps: { include: ['react', 'react-dom/client', 'react/jsx-runtime', 'react-i18next', 'i18next'], noDiscovery: true },
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{ name: 'composer-label-fixture', configureServer(vite) {
    vite.middlewares.use(async (req, res, next) => {
      if (req.url !== '/composer-fixture.html') return next();
      res.setHeader('Content-Type', 'text/html');
      res.end(await vite.transformIndexHtml(req.url, html));
    });
  } }],
});
let browser;
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1100, height: 500 } });
  page.on('pageerror', error => { errors.push(error.message); console.error('[pageerror]', error.message); });
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/composer-fixture.html`, { waitUntil: 'networkidle' });
  await page.locator('.ace-composer-model-label').waitFor();
  await page.waitForLoadState('networkidle');
  const set = async patch => {
    await page.evaluate(patch => window.updateFixture(patch), patch);
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(100);
    return page.evaluate(() => {
      const control = document.querySelector('[data-composer-control="model"]');
      const label = control.querySelector('.ace-composer-model-label');
      const footer = document.querySelector('.ace-composer-session-footer');
      const right = footer.querySelector('.ace-composer-session-right').getBoundingClientRect();
      const leftEdge = Math.max(...[...footer.querySelector('.ace-composer-session-left').children]
        .map(element => element.getBoundingClientRect().right));
      return { compact: control.dataset.compact, text: label.textContent,
        labelVisible: getComputedStyle(label).display !== 'none' && label.getBoundingClientRect().width > 0,
        clipped: label.scrollWidth > label.clientWidth + 1,
        overlap: leftEdge > right.left,
        leftEdge, rightLeft: right.left, rootWidth: footer.getBoundingClientRect().width, rightWidth: right.width,
        inlineWidth: control.style.width, flex: control.style.flex,
        controls: [...footer.querySelector('.ace-composer-session-left').children].map(element => ({
          name: element.dataset.composerControl, compact: element.dataset.compact,
          width: element.getBoundingClientRect().width, right: element.getBoundingClientRect().right,
        })),
        overflow: footer.scrollWidth > footer.clientWidth + 1,
        width: control.getBoundingClientRect().width };
    });
  };
  for (const name of ['ACEModel-gpt-6-astra', 'ACEModel-a-much-longer-model-name-with-a-version-suffix']) {
    let result = await set({ width: 850, name, extras: false });
    assert.equal(result.compact, 'false', JSON.stringify(result));
    assert.equal(result.clipped, false, JSON.stringify(result));
    assert.equal(result.text, name);
    assert.equal(result.text.includes('backend-model'), false);
    assert.equal(result.overflow, false);
    await page.screenshot({ path: path.join(output, name.length > 25 ? 'wide-long.png' : 'wide.png') });
    for (const width of [480, 360, 320]) {
      result = await set({ width, extras: true });
      assert.equal(result.labelVisible, true, JSON.stringify(result));
      assert.equal(result.overlap, false, JSON.stringify({ containerWidth: width, ...result }));
      assert.equal(result.overflow, false, JSON.stringify({ containerWidth: width, ...result }));
    }
    assert.equal(result.compact, 'true', JSON.stringify(result));
    assert.equal(result.clipped, true, JSON.stringify(result));
    await page.screenshot({ path: path.join(output, 'narrow.png') });
    result = await set({ width: 850, extras: false });
    assert.equal(result.compact, 'false');
    assert.equal(result.clipped, false);
  }
  await page.locator('.ace-composer-model-button').click();
  await page.locator('.ace-composer-model-options').waitFor();
  assert.equal(await page.locator('.ace-composer-model-option').count(), 1);
  assert.match(await page.locator('.ace-composer-model-option').innerText(), /backend-model/);
  const openMenuResize = await set({ width: 900 });
  assert.equal(openMenuResize.clipped, false);
  await page.keyboard.press('Escape');
  await page.setViewportSize({ width: 480, height: 500 });
  const mobile = await set({ width: 400, extras: false, name: 'ACEModel-gpt-6-astra' });
  assert.equal(mobile.compact, 'false', JSON.stringify(mobile));
  assert.equal(mobile.clipped, false, JSON.stringify(mobile));
  await page.screenshot({ path: path.join(output, 'mobile-full-name.png') });
  assert.deepEqual(errors, []);
  console.log('[pass] production composer keeps full names, clips only when constrained, restores on resize, and preserves menu details');
  console.log(`Screenshots: ${output}`);
} finally {
  await browser?.close();
  await server.close();
}
