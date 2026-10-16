import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const output = await fs.mkdtemp(path.join(os.tmpdir(), 'ace-expert-sampling-'));
const fixture = `
import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import { ExpertEditor } from '/src/components/ExpertEditor.jsx';
import { emptyExpertForm, expertFormFromDetail } from '/src/lib/expertComponents.js';
import { api } from '/src/lib/api.js';
import '/src/styles/globals.css';
api.listExpertCapabilities = async () => ({});
api.updateExpert = async (id, payload) => {
  window.saved = payload;
  window.saveCount = (window.saveCount || 0) + 1;
  return { ...payload, agents: [payload.lead], lead_agent_id: 'lead' };
};
function Fixture() {
  const [version, setVersion] = useState(0);
  const [initial, setInitial] = useState(() => ({ ...emptyExpertForm(),
    displayName: '代码审查专家', instructions: '检查实现并说明验证结果' }));
  return <ExpertEditor key={version} editing initial={initial} onClose={() => {}}
    onSaved={detail => { setInitial(expertFormFromDetail(detail)); setVersion(v => v + 1); }} />;
}
createRoot(document.getElementById('root')).render(<Fixture />);
`;
const virtual = path.join(web, '__expert-sampling-fixture.jsx').replaceAll('\\', '/');
const server = await createServer({ root: web,
  server: { host: '127.0.0.1', port: 0, open: false, hmr: false },
  plugins: [{ name: 'expert-sampling-fixture', enforce: 'pre',
    resolveId(id) { return id === '/__expert-sampling-fixture.jsx' ? virtual : undefined; },
    load(id) { return id.replaceAll('\\', '/') === virtual ? fixture : undefined; },
    configureServer(vite) { vite.middlewares.use(async (req, res, next) => {
      if (req.url !== '/sampling-fixture') return next();
      res.setHeader('Content-Type', 'text/html');
      res.end(await vite.transformIndexHtml('/sampling-fixture', '<!doctype html><html data-theme="light"><meta name="viewport" content="width=device-width, initial-scale=1"><body><div id="root"></div><script type="module" src="/__expert-sampling-fixture.jsx"></script></body></html>'));
    }); },
  }],
});
let browser;
const errors = [];
try {
  await server.listen();
  browser = await chromium.launch({ channel: 'msedge', headless: true });
  const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/sampling-fixture`);
  const fields = ['Temperature', 'Top P', 'Top K', 'Min P'];
  const input = label => page.getByRole('spinbutton', { name: new RegExp(`^${label}`) });
  for (const [i, value] of ['0', '0.95', '20', '0'].entries()) await input(fields[i]).fill(value);
  await page.getByRole('button', { name: '保存专家', exact: true }).click();
  await page.waitForFunction(() => window.saveCount === 1);
  assert.deepEqual(await page.evaluate(() => window.saved.sampling), { temperature: 0, top_p: 0.95, top_k: 20, min_p: 0 });
  for (const [i, value] of ['0', '0.95', '20', '0'].entries()) assert.equal(await input(fields[i]).inputValue(), value);
  await page.locator('[data-expert-sampling]').scrollIntoViewIfNeeded();
  await page.screenshot({ path: path.join(output, 'desktop-light.png') });
  await input('Top K').fill('1.5');
  await page.getByRole('button', { name: '保存专家', exact: true }).click();
  assert.equal(await input('Top K').getAttribute('aria-invalid'), 'true');
  assert.equal(await page.evaluate(() => window.saveCount), 1);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.evaluate(() => document.documentElement.dataset.theme = 'dark');
  await page.locator('[data-expert-sampling]').scrollIntoViewIfNeeded();
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
  for (const label of fields) {
    const box = await input(label).boundingBox();
    assert.ok(box.x >= 0 && box.x + box.width <= 390, `${label} stays inside narrow viewport`);
  }
  await page.screenshot({ path: path.join(output, 'narrow-dark-validation.png') });
  for (const label of fields) await input(label).fill('');
  await page.getByRole('button', { name: '保存专家', exact: true }).click();
  await page.waitForFunction(() => window.saveCount === 2);
  assert.deepEqual(await page.evaluate(() => window.saved.sampling), {});
  for (const label of fields) assert.equal(await input(label).inputValue(), '');
  assert.deepEqual(errors, []);
  console.log(JSON.stringify({ ok: true, output, cases: ['save', 'zero', 'reopen', 'invalid', 'clear', 'narrow-dark'] }));
} finally {
  await browser?.close();
  await server.close();
}
