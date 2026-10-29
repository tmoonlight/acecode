import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const web = fileURLToPath(new URL('../', import.meta.url));
const { chromium } = await import(process.env.ACE_PLAYWRIGHT_MODULE
  ? pathToFileURL(path.resolve(process.env.ACE_PLAYWRIGHT_MODULE)).href : 'playwright');
const reproduce = process.argv.includes('--reproduce');
const fixture = String.raw`
import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import { PreviewDetailsPanel } from '/src/components/PreviewDetailsPanel.jsx';
import { PREVIEW_TAB_TYPES } from '/src/lib/previewTabs.js';
import { createSelectionContext } from '/src/lib/selectionChatContext.js';
import { GIT_STATE_CHANGED_EVENT } from '/src/lib/gitSessionPill.js';
import '/src/styles/globals.css';

let text = 'alpha beta gamma\n' + 'following line\n'.repeat(180);
let patch = 'diff --git a/source.txt b/source.txt\n--- a/source.txt\n+++ b/source.txt\n@@ -1 +1 @@\n-old text\n+alpha beta gamma\n';
let pending = [], hold = false, fail = false, reads = 0, patchReads = 0;
const result = value => hold ? new Promise((resolve, reject) => pending.push(() => fail ? reject(Error('fixture failure')) : resolve(value))) : (fail ? Promise.reject(Error('fixture failure')) : Promise.resolve(value));
const api = {
  readFile: () => { reads++; return result(text); },
  readEditableFile: () => { reads++; return result({ text, read_id: 'read-' + reads, size: text.length }); },
  gitChanges: () => result({ files: [{ path: 'source.txt', status: 'M', additions: 1, deletions: 1 }], total_count: 1 }),
  gitFileDiff: () => { patchReads++; return result({ patch }); },
};
function Fixture() {
  const [tab, setTab] = useState({ key: 'source.txt', type: PREVIEW_TAB_TYPES.FILE, path: 'source.txt' });
  const [tick, setTick] = useState(0), [busy, setBusy] = useState(true), [marked, setMarked] = useState(true);
  const contexts = marked ? [createSelectionContext({ text: 'beta', path: 'C:/fixture/' + tab.path,
    startLine: 1, endLine: 1, startOffset: 6, endOffset: 10,
    view: tab.path?.endsWith('.md') ? 'rendered' : 'source' }),
    createSelectionContext({ text: String(tick), path: 'C:/fixture/unrelated.txt' })] : [];
  window.fixture = {
    get reads() { return reads; }, get patchReads() { return patchReads; }, get pending() { return pending.length; }, get tab() { return tab; },
    tick: () => setTick(n => n + 1), busy: setBusy, marked: setMarked,
    text: value => { text = value; }, patch: value => { patch = value; },
    hold: value => { hold = value; }, fail: value => { fail = value; },
    release: () => { const batch = pending; pending = []; batch.forEach(f => f()); },
    refresh: () => setTab(t => ({ ...t, reloadRevision: (t.reloadRevision || 0) + 1 })),
    select: (name, desktop = false) => { window.__ACECODE_DESKTOP_SHELL__ = desktop; setTab({ key: name, type: PREVIEW_TAB_TYPES.FILE, path: name }); },
    git: () => setTab({ key: 'git', type: PREVIEW_TAB_TYPES.GIT_CHANGES, base: 'HEAD', expandedFile: 'source.txt' }),
    base: base => setTab(t => ({ ...t, base })),
    notifyGit: () => window.dispatchEvent(new CustomEvent(GIT_STATE_CHANGED_EVENT, { detail: { cwd: 'C:/fixture' } })),
  };
  return <main style={{ display: 'flex', height: '100vh' }} data-tick={tick}>
    <PreviewDetailsPanel owner="selection-stability" api={api} cwd="C:/fixture" tabs={[tab]} activeTab={tab}
      busy={busy} selectionContexts={contexts}
      onEditFileTab={(_key, patch) => setTab(t => {
        const edit = { ...t.edit, ...patch };
        return { ...t, edit: { ...edit, dirty: String(edit.text ?? '') !== String(edit.baselineText ?? '') } };
      })} />
  </main>;
}
createRoot(document.getElementById('root')).render(<Fixture />);
`;
const virtual = path.join(web, '__selection-stability.jsx').replaceAll('\\', '/');
const html = `<html data-theme="light"><body style="margin:0"><div id="root"></div>
<script type="module">import RefreshRuntime from '/@react-refresh';RefreshRuntime.injectIntoGlobalHook(window);
window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;window.__vite_plugin_react_preamble_installed__=true;</script>
<script type="module" src="/__selection-stability.jsx"></script></body></html>`;
const server = await createServer({ root: web, configFile: path.join(web, 'vite.config.js'), logLevel: 'error',
  plugins: [{ name: 'selection-stability-fixture', enforce: 'pre',
    resolveId(id) { if (id === '/__selection-stability.jsx') return virtual; },
    load(id) { if (id.replaceAll('\\', '/') === virtual) return fixture; },
    configureServer(vite) { vite.middlewares.use((req, res, next) => {
      if (req.url?.split('?')[0] !== '/__selection-stability') return next();
      res.setHeader('Content-Type', 'text/html'); res.end(html);
    }); },
  }], server: { host: '127.0.0.1', port: 0, hmr: false },
});
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true, ...(process.env.ACE_CHROMIUM_EXECUTABLE ? { executablePath: process.env.ACE_CHROMIUM_EXECUTABLE } : {}) });
  const page = await browser.newPage({ viewport: { width: 1050, height: 700 } });
  const errors = [];
  page.on('pageerror', error => { errors.push(error.message); console.error(error.message); });
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/__selection-stability`);
  const settle = () => page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  const select = async (selector) => {
    await page.locator(selector).first().waitFor();
    return page.locator(selector).first().evaluate(element => {
      const walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
      let node;
      while ((node = walker.nextNode())) {
        if (!node.textContent.trim()) continue;
        const range = document.createRange(); range.selectNodeContents(node);
        const selection = window.getSelection(); selection.removeAllRanges(); selection.addRange(range);
        window.selectedNode = node; window.selectedText = selection.toString();
        return window.selectedText;
      }
      throw Error('missing selectable text');
    });
  };
  const retained = () => page.evaluate(() => window.selectedNode.isConnected && window.getSelection().toString() === window.selectedText);
  const expectRetained = async name => {
    assert.equal(await retained(), !reproduce, name);
    console.log(`[${reproduce ? 'reproduced' : 'pass'}] ${name}`);
  };

  // 流式输出创建等值的新 context 对象，不能拆建已有 mark 内的选区节点。
  await select('.ace-selection-reference-mark');
  await page.evaluate(() => window.fixture.tick()); await settle();
  await expectRetained('equivalent streaming selection contexts');
  if (!reproduce) {
    for (let index = 0; index < 3; index++) {
      await page.evaluate(() => window.fixture.tick()); await settle();
      assert.ok(await retained(), 'repeated stream updates and references to other files leave selection intact');
    }
  }

  await select('.ace-selection-reference-mark');
  await page.evaluate(() => { window.fixture.hold(true); window.fixture.refresh(); }); await settle();
  await expectRetained('same-file refresh keeps selection while request is pending');
  await page.evaluate(() => { window.fixture.hold(false); window.fixture.release(); }); await settle();
  if (!reproduce) assert.ok(await retained(), 'identical response preserves original text node and selection');

  await page.evaluate(() => window.fixture.git());
  await settle();
  await select('.d2h-code-line-ctn');
  await page.evaluate(() => { window.fixture.hold(true); window.fixture.notifyGit(); }); await settle();
  await expectRetained('Git refresh keeps loaded patch selection');
  await page.evaluate(() => { window.fixture.hold(false); window.fixture.release(); }); await settle();
  await page.waitForFunction(() => document.querySelector('.d2h-code-line-ctn'));
  if (!reproduce) {
    assert.ok(await retained(), 'identical Git response preserves nodes');
    const before = await page.evaluate(() => window.fixture.patchReads);
    await page.evaluate(() => window.fixture.busy(false)); await settle();
    await page.waitForFunction(n => window.fixture.patchReads > n, before);
    assert.ok(await retained(), 'turn completion preserves identical patch');
    await page.evaluate(() => {
      window.fixture.patch('diff --git a/source.txt b/source.txt\n--- a/source.txt\n+++ b/source.txt\n@@ -1 +1 @@\n-old text\n+changed actual content\n');
      window.fixture.refresh();
    });
    await page.waitForFunction(() => document.querySelector('.ace-review-diff')?.textContent.includes('changed actual content'));
    console.log('[pass] Git refresh loads changed patch and turn completion preserves unchanged patch');

    await page.evaluate(() => { window.fixture.hold(true); window.fixture.base('other-base'); }); await settle();
    assert.equal(await page.locator('.ace-review-diff').count(), 0, 'new comparison never shows old patch');
    await page.evaluate(() => { window.fixture.hold(false); window.fixture.release(); }); await settle();

    await page.evaluate(() => { window.fixture.marked(false); window.fixture.select('plain.txt'); });
    await select('.ace-line-code');
    await page.evaluate(() => { window.fixture.hold(true); window.fixture.refresh(); }); await settle();
    assert.ok(await retained());
    await page.locator('.ace-preview').evaluate(element => { element.scrollTop = 200; });
    await page.evaluate(() => { window.fixture.hold(false); window.fixture.release(); }); await settle();
    assert.equal(await page.locator('.ace-preview').evaluate(element => element.scrollTop), 200, 'refresh does not revert user scroll');
    assert.ok(await retained());
    await page.evaluate(() => { window.fixture.text('changed file content'); window.fixture.refresh(); });
    await page.waitForFunction(() => document.querySelector('.ace-line-code')?.textContent === 'changed file content');
    assert.equal(await page.locator('.ace-preview').evaluate(element => element.scrollTop), 0);
    await page.evaluate(() => { window.fixture.text(''); window.fixture.refresh(); });
    await page.waitForFunction(() => document.querySelector('.ace-line-code')?.textContent === ' ');
    console.log('[pass] plain text selection, scrolling during read, changed and empty files');

    await page.evaluate(() => { window.fixture.text('alpha beta gamma'); window.fixture.select('article.md'); window.fixture.marked(true); });
    await select('.ace-selection-reference-mark');
    await page.evaluate(() => window.fixture.tick()); await settle(); assert.ok(await retained());
    await page.evaluate(() => window.fixture.refresh()); await settle(); assert.ok(await retained());
    console.log('[pass] rendered Markdown selection survives streaming and identical refresh');

    await page.evaluate(() => { window.fixture.marked(false); window.fixture.select('editable.txt', true); });
    const editor = page.locator('.ace-file-text-editor'); await editor.waitFor();
    const oldReadId = await page.evaluate(() => window.fixture.tab.edit.readId);
    await editor.evaluate(element => { element.focus(); element.setSelectionRange(2, 8); window.savedEditor = element; });
    await page.evaluate(() => window.fixture.refresh()); await settle();
    assert.equal(await page.evaluate(() => window.savedEditor === document.querySelector('.ace-file-text-editor')), true);
    assert.deepEqual(await editor.evaluate(element => [element.selectionStart, element.selectionEnd]), [2, 8]);
    assert.notEqual(await page.evaluate(() => window.fixture.tab.edit.readId), oldReadId);
    console.log('[pass] editable selection and fresh read credentials');

    await page.evaluate(() => { window.fixture.hold(true); window.fixture.refresh(); }); await settle();
    await editor.fill('draft typed during refresh');
    await page.evaluate(() => { window.fixture.hold(false); window.fixture.release(); }); await settle();
    assert.equal(await editor.inputValue(), 'draft typed during refresh');
    assert.equal(await page.evaluate(() => window.fixture.tab.edit.dirty), true);
    console.log('[pass] background read never overwrites a newer unsaved draft');

    await page.evaluate(() => { window.fixture.select('race.txt'); }); await page.locator('.ace-preview').waitFor();
    await page.evaluate(() => { window.fixture.text('stale response'); window.fixture.hold(true); window.fixture.refresh(); }); await settle();
    await page.evaluate(() => { window.fixture.hold(false); window.fixture.text('new file'); window.fixture.select('new.txt'); });
    await page.waitForFunction(() => document.querySelector('.ace-line-code')?.textContent === 'new file');
    await page.evaluate(() => window.fixture.release()); await settle();
    assert.equal(await page.locator('.ace-line-code').textContent(), 'new file');
    await page.evaluate(() => { window.fixture.fail(true); window.fixture.refresh(); });
    await page.waitForFunction(() => document.querySelector('[data-desktop-preview-path="new.txt"]')?.textContent.includes('读取失败'));
    console.log('[pass] stale file response ignored and read failures visible');
  }
  assert.deepEqual(errors, []);
} finally {
  await browser?.close();
  await server.close();
}
